/*
    This file is part of AutoQuad.

    AutoQuad is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    AutoQuad is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with AutoQuad.  If not, see <http://www.gnu.org/licenses/>.

    Copyright � 2011, 2012, 2013  Bill Nesbitt
*/

#include "aq.h"
#include "motors.h"
#include "radio.h"
#include "util.h"
#include "config.h"
#include "comm.h"
#include "aq_timer.h"
#include "rcc.h"
#include "analog.h"
#include "aq_mavlink.h"
#include "can.h"
#include "esc32.h"
#include "supervisor.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

motorsStruct_t motorsData __attribute__((section(".ccm")));

static void motorsCanSendGroups(void) {
    int i;

    for (i = 0; i < motorsData.numGroups; i++)
	canCommandSetpoint16(i+1, (uint8_t *)&motorsData.canGroups[i]);
}

void motorsSendValues(void) {
    int i;

    for (i = 0; i < MOTORS_NUM; i++)
	if (motorsData.active[i]) {
	    // ensure motor output is constrained
	    motorsData.value[i] = constrainInt(motorsData.value[i], 0, MOTORS_SCALE);

	    // PWM
	    if (i < PWM_NUM_PORTS && motorsData.pwm[i]) {
		if (supervisorData.state & STATE_ARMED)
		    *motorsData.pwm[i]->ccr = constrainInt((float)motorsData.value[i] * (p[MOT_MAX] -  p[MOT_MIN]) / MOTORS_SCALE + p[MOT_MIN], p[MOT_START], p[MOT_MAX]);
		else
		    *motorsData.pwm[i]->ccr = 0;
	    }
	    // CAN
	    else if (motorsData.can[i]) {
		if (supervisorData.state & STATE_ARMED)
		    // convert to 16 bit
		    *motorsData.canPtrs[i] = constrainInt(motorsData.value[i], MOTORS_SCALE * 0.1f, MOTORS_SCALE)<<4;
		else
		    *motorsData.canPtrs[i] = 0;
	    }
	}

    motorsCanSendGroups();
}

void motorsOff(void) {
    int i;

    for (i = 0; i < MOTORS_NUM; i++)
	if (motorsData.active[i]) {
	    motorsData.value[i] = 0;

	    // PWM
	    if (i < PWM_NUM_PORTS && motorsData.pwm[i])
		*motorsData.pwm[i]->ccr = (supervisorData.state & STATE_ARMED) ? p[MOT_ARM] : 0;
	    // CAN
	    else if (motorsData.can[i])
		*motorsData.canPtrs[i] = 0;
	}

    motorsCanSendGroups();

    motorsData.throttle = 0;
    motorsData.throttleLimiter = 0.0f;
}

void motorsCommands(float throtCommand, float pitchCommand, float rollCommand, float ruddCommand) {
    float throttle;
    float voltageFactor;
    float value;
    float nominalBatVolts;
    int i;

    // throttle limiter to prevent control saturation
    throttle = constrainFloat(throtCommand - motorsData.throttleLimiter, 0.0f, MOTORS_SCALE);

    // calculate voltage factor
    nominalBatVolts = MOTORS_CELL_VOLTS*analogData.batCellCount;
    voltageFactor = 1.0f + (nominalBatVolts - analogData.vIn) / nominalBatVolts;

    // calculate and set each motor value
    for (i = 0; i < MOTORS_NUM; i++) {
	if (motorsData.active[i]) {
	    motorsPowerStruct_t *d = &motorsData.distribution[i];

	    value = 0.0f;
	    value += (throttle * d->throttle * 0.01f);
	    value += (pitchCommand * d->pitch * 0.01f);
	    value += (rollCommand * d->roll * 0.01f);
	    value += (ruddCommand * d->yaw * 0.01f);

	    value *= voltageFactor;

	    // check for over throttle
	    if (value >= MOTORS_SCALE)
		motorsData.throttleLimiter += MOTORS_THROTTLE_LIMITER;

	    motorsData.value[i] = constrainInt(value, 0, MOTORS_SCALE);
	}
    }

    motorsSendValues();

    // decay throttle limit
    motorsData.throttleLimiter = constrainFloat(motorsData.throttleLimiter - MOTORS_THROTTLE_LIMITER, 0.0f, MOTORS_SCALE/4);

    motorsData.pitch = pitchCommand;
    motorsData.roll = rollCommand;
    motorsData.yaw = ruddCommand;
    motorsData.throttle = throttle;
}

static void motorsCanInit(int i) {
    if ((motorsData.can[i] = canFindNode(CAN_TYPE_ESC, i+1)) == 0) {
	AQ_PRINTF("Motors: cannot find CAN id [%d]\n", i+1);
    }
    else {
#ifdef USE_L1_ATTITUDE
	esc32SetupCan(motorsData.can[i], 1);
#else
	esc32SetupCan(motorsData.can[i], 0);
#endif
    }
}

static void motorsPwmInit(int i) {
#ifdef USE_L1_ATTITUDE
    motorsData.pwm[i] = pwmInitOut(i, PWM_PRESCALE/MOTORS_PWM_FREQ, 0, 1);	    // closed loop RPM mode
#else
    motorsData.pwm[i] = pwmInitOut(i, PWM_PRESCALE/MOTORS_PWM_FREQ, 0, 0);	    // open loop mode
#endif
}

void motorsArm(void) {
    int i;

    // group arm
    for (i = 0; i < motorsData.numGroups; i++)
	canCommandArm(CAN_TT_GROUP, i+1);

    // wait for all to arm
    for (i = 0; i < MOTORS_NUM; i++)
	if (motorsData.can[i])
	    while (*canGetState(motorsData.can[i]->nodeId) == ESC32_STATE_DISARMED)
		yield(1);
}

void motorsDisarm(void) {
    int i;

    // group disarm
    for (i = 0; i < motorsData.numGroups; i++)
	canCommandDisarm(CAN_TT_GROUP, i+1);
}

static void motorsSetCanGroup(void) {
    int group;
    int subGroup;
    int i;

    group = 0;
    subGroup = 0;
    for (i = 0; i < MOTORS_NUM; i++) {
	if (motorsData.can[i]) {
	    canSetGroup(motorsData.can[i]->nodeId, group+1, subGroup+1);

	    switch (subGroup) {
		case 0:
		    motorsData.canPtrs[i] = &motorsData.canGroups[group].value1;
		    motorsData.numGroups++;
		    break;
		case 1:
		    motorsData.canPtrs[i] = &motorsData.canGroups[group].value2;
		    break;
		case 2:
		    motorsData.canPtrs[i] = &motorsData.canGroups[group].value3;
		    break;
		case 3:
		    motorsData.canPtrs[i] = &motorsData.canGroups[group].value4;
		    break;
	    }

	    subGroup++;
	    if (subGroup == MOTORS_CAN_GROUP_SIZE) {
		group++;
		subGroup = 0;
	    }
	}
    }
}

void motorsInit(void) {
    float sumPitch, sumRoll, sumYaw;
    int i;

    AQ_NOTICE("Motors init\n");

    memset((void *)&motorsData, 0, sizeof(motorsData));

    if (p[MOT_FRAME] > 0.01f && p[MOT_FRAME] < 4.01f) {
	AQ_NOTICE("Motors: ERROR! Predefined frame types are no longer supported.\n");
	return;
    }

    motorsData.distribution = (motorsPowerStruct_t *)&p[MOT_PWRD_01_T];

    sumPitch = 0.0f;
    sumRoll = 0.0f;
    sumYaw = 0.0f;

    for (i = 0; i < MOTORS_NUM; i++) {
	motorsPowerStruct_t *d = &motorsData.distribution[i];

	if (d->throttle != 0.0f || d->pitch != 0.0f || d->roll != 0.0f || d->yaw != 0.0f) {

	    // CAN
	    if (((uint32_t)p[MOT_CAN]) & (1<<i))
		motorsCanInit(i);
	    // PWM
	    else if (i < PWM_NUM_PORTS)
		motorsPwmInit(i);

	    motorsData.active[i] = 1;

	    sumPitch += d->pitch;
	    sumRoll += d->roll;
	    sumYaw += d->yaw;
	}
    }

    if (fabsf(sumPitch) > 0.01f)
	AQ_NOTICE("Motors: Warning pitch control imbalance\n");

    if (fabsf(sumRoll) > 0.01f)
	AQ_NOTICE("Motors: Warning roll control imbalance\n");

    if (fabsf(sumYaw) > 0.01f)
	AQ_NOTICE("Motors: Warning yaw control imbalance\n");

    motorsSetCanGroup();
    motorsOff();
}