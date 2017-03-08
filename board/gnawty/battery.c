/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery pack vendor provided charging profile
 */

#include "battery.h"
#include "battery_smart.h"
#include "console.h"
#include "gpio.h"
#include "host_command.h"
#include "system.h"
#include "util.h"

#define CPRINTF(format, args...) cprintf(CC_CHIPSET, format, ## args)

/* Shutdown mode parameter to write to manufacturer access register */
#define	SB_SHIP_MODE_ADDR	0x3a
#define	SB_SHIP_MODE_DATA	0xc574

static const struct battery_info info_AC14 = {
	.voltage_max    = 12900,		/* mV */
	.voltage_normal = 11400,
	.voltage_min    = 9000,
	.precharge_current  = 256,	/* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 50,
	.charging_min_c       = 0,
	.charging_max_c       = 60,
	.discharging_min_c    = 0,
	.discharging_max_c    = 75,
};

static const struct battery_info info_AC15 = {
	/* New battery, use BOARD_ID pin 3 tp separate it. */
	.voltage_max	= 12600,	/* mV */
	.voltage_normal = 10800,
	.voltage_min	= 8250,
	.precharge_current  = 340,	/* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 50,
	.charging_min_c       = 0,
	.charging_max_c       = 60,
	.discharging_min_c    = -20,
	.discharging_max_c    = 75,
};

static const struct battery_info info_AC14B3K = {
	/* New battery, use BOARD_ID pin 2 tp separate it. */
	.voltage_max	= 17600,	/* mV */
	.voltage_normal = 15400,
	.voltage_min	= 12000,
	.precharge_current  = 340,	/* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 50,
	.charging_min_c       = 0,
	.charging_max_c       = 60,
	.discharging_min_c    = -20,
	.discharging_max_c    = 60,
};

const struct battery_info *battery_get_info(void)
{
	int board_version = 0;

	board_version = system_get_board_version();

	/*
	 * This system supports multiple batteries:
	 * AC14 - The original, only on boards with id 0.
	 * AC15 - Second battery, on boards with only the third id bit set.
	 * AC14BK - Third battery, on boards with only the second id bit set.
	 */
	switch (board_version) {
	case 0x00:
		return &info_AC14;
	case 0x02:
		return &info_AC14B3K;
	case 0x04:
		return &info_AC15;
	default:
		CPRINTF("Invalid Board ID: battery configuration load failed");
		ASSERT(0);
	}
	/* We should never get here. */
	return NULL;
}

static int battery_command_cut_off(struct host_cmd_handler_args *args)
{
	return sb_write(SB_SHIP_MODE_ADDR, SB_SHIP_MODE_DATA);
}
DECLARE_HOST_COMMAND(EC_CMD_BATTERY_CUT_OFF, battery_command_cut_off,
		     EC_VER_MASK(0));

static int command_battcutoff(int argc, char **argv)
{
	return sb_write(SB_SHIP_MODE_ADDR, SB_SHIP_MODE_DATA);
}
DECLARE_CONSOLE_COMMAND(battcutoff, command_battcutoff,
			NULL,
			"Enable battery cutoff (ship mode)",
			NULL);