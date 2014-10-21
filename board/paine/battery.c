/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery pack vendor provided charging profile
 */

#include "battery.h"
#include "battery_smart.h"
#include "gpio.h"
#include "host_command.h"

#define SB_SHIP_MODE_ADDR	0x3a
#define SB_SHIP_MODE_DATA	0xc574

/* Values for 45W 3UAF576790-1-T1183 & LIS3105ACPC(SY6) batteries */
static const struct battery_info info = {

	.voltage_max    = 13050,
	.voltage_normal = 11025, /* Average of max & min */
	.voltage_min    =  9000,

	/* Pre-charge values. */
	.precharge_current  = 256,	/* mA */

	.start_charging_min_c = 0,
	.start_charging_max_c = 50,
	.charging_min_c       = 0,
	.charging_max_c       = 60,
	.discharging_min_c    = 0,
	.discharging_max_c    = 60,
};

const struct battery_info *battery_get_info(void)
{
	return &info;
}

int board_cut_off_battery(void)
{
	return sb_write(SB_SHIP_MODE_ADDR, SB_SHIP_MODE_DATA);
}
