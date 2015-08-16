/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* I2C port module for Chrome EC */

#include "board.h"
#include "clock.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "task.h"
#include "timer.h"
#include "registers.h"
#include "util.h"

#define CPUTS(outstr) cputs(CC_I2C, outstr)
#define CPRINTF(format, args...) cprintf(CC_I2C, format, ## args)

#define NUM_PORTS 6  /* Number of physical ports */

/* Flags for writes to MCS */
#define LM4_I2C_MCS_RUN   (1 << 0)
#define LM4_I2C_MCS_START (1 << 1)
#define LM4_I2C_MCS_STOP  (1 << 2)
#define LM4_I2C_MCS_ACK   (1 << 3)
#define LM4_I2C_MCS_HS    (1 << 4)
#define LM4_I2C_MCS_QCMD  (1 << 5)

/* Flags for reads from MCS */
#define LM4_I2C_MCS_BUSY   (1 << 0)
#define LM4_I2C_MCS_ERROR  (1 << 1)
#define LM4_I2C_MCS_ADRACK (1 << 2)
#define LM4_I2C_MCS_DATACK (1 << 3)
#define LM4_I2C_MCS_ARBLST (1 << 4)
#define LM4_I2C_MCS_IDLE   (1 << 5)
#define LM4_I2C_MCS_BUSBSY (1 << 6)
#define LM4_I2C_MCS_CLKTO  (1 << 7)

#define START 1
#define STOP  1
#define NO_START 0
#define NO_STOP  0

/* Delay for bitbanging i2c corresponds roughly to 100kHz. */
#define I2C_BITBANG_DELAY_US    5

/* Number of attempts to unwedge each pin. */
#define UNWEDGE_SCL_ATTEMPTS  10
#define UNWEDGE_SDA_ATTEMPTS  3

static task_id_t task_waiting_on_port[NUM_PORTS];
static struct mutex port_mutex[NUM_PORTS];
extern const struct i2c_port_t i2c_ports[I2C_PORTS_USED];


static int wait_idle(int port)
{
	int i;
	int event = 0;

	i = LM4_I2C_MCS(port);
	while (i & LM4_I2C_MCS_BUSY) {
		/* Port is busy, so wait for the interrupt */
		task_waiting_on_port[port] = task_get_current();
		LM4_I2C_MIMR(port) = 0x03;
		/* We want to wait here quietly until the I2C interrupt comes
		 * along, but we don't want to lose any pending events that
		 * will be needed by the task that started the I2C transaction
		 * in the first place. So we save them up and restore them when
		 * the I2C is either completed or timed out. Refer to the
		 * implementation of usleep() for a similar situation.
		 */
		event |= (task_wait_event(1000000) & ~TASK_EVENT_I2C_IDLE);
		LM4_I2C_MIMR(port) = 0x00;
		task_waiting_on_port[port] = TASK_ID_INVALID;
		if (event & TASK_EVENT_TIMER) {
			/* Restore any events that we saw while waiting */
			task_set_event(task_get_current(),
				       (event & ~TASK_EVENT_TIMER), 0);
			return EC_ERROR_TIMEOUT;
		}

		i = LM4_I2C_MCS(port);
	}

	/* Restore any events that we saw while waiting. TASK_EVENT_TIMER isn't
	 * one, because we've handled it above.
	 */
	task_set_event(task_get_current(), event, 0);

	/* Check for errors */
	if (i & (LM4_I2C_MCS_CLKTO | LM4_I2C_MCS_ARBLST | LM4_I2C_MCS_ERROR))
		return EC_ERROR_UNKNOWN;

	return EC_SUCCESS;
}


/* Transmit one block of raw data, then receive one block of raw data.
 * <start> flag indicates this smbus session start from idle state.
 * <stop>  flag means this session can be termicate with smbus stop bit
 */
static int i2c_transmit_receive(int port, int slave_addr,
		uint8_t *transmit_data, int transmit_size,
		uint8_t *receive_data, int receive_size,
		int start, int stop)
{
	int rv, i;
	int started = start ? 0 : 1;
	uint32_t reg_mcs;

	if (transmit_size == 0 && receive_size == 0)
		return EC_SUCCESS;

	reg_mcs = LM4_I2C_MCS(port);
	if (!started &&
	    ((reg_mcs & (LM4_I2C_MCS_CLKTO | LM4_I2C_MCS_ARBLST)) ||
	     (i2c_get_line_levels(port) != I2C_LINE_IDLE))) {
		uint32_t tpr = LM4_I2C_MTPR(port);

		CPRINTF("I2C%d Addr:%02X bad status 0x%02x, SCL=%d, SDA=%d\n",
				port,
				slave_addr,
				reg_mcs,
				i2c_get_line_levels(port) & I2C_LINE_SCL_HIGH,
				i2c_get_line_levels(port) & I2C_LINE_SDA_HIGH);

		/* Attempt to unwedge the port. */
		i2c_unwedge(port);

		/* Clock timeout or arbitration lost.  Reset port to clear. */
		LM4_SYSTEM_SRI2C |= (1 << port);
		clock_wait_cycles(3);
		LM4_SYSTEM_SRI2C &= ~(1 << port);
		clock_wait_cycles(3);

		/* Restore settings */
		LM4_I2C_MCR(port) = 0x10;
		LM4_I2C_MTPR(port) = tpr;

		/*
		 * We don't know what edges the slave saw, so sleep long enough
		 * that the slave will see the new start condition below.
		 */
		usleep(1000);
	}

	if (transmit_data) {
		LM4_I2C_MSA(port) = slave_addr & 0xff;
		for (i = 0; i < transmit_size; i++) {
			LM4_I2C_MDR(port) = transmit_data[i];
			/* Setup master control/status register
			 * MCS sequence on multi-byte write:
			 *     0x3 0x1 0x1 ... 0x1 0x5
			 * Single byte write:
			 *     0x7
			 */
			reg_mcs = LM4_I2C_MCS_RUN;
			/* Set start bit on first byte */
			if (!started) {
				started = 1;
				reg_mcs |= LM4_I2C_MCS_START;
			}
			/* Send stop bit if the stop flag is on,
			 * and caller doesn't expect to receive
			 * data.
			 */
			if (stop && receive_size == 0 && i ==
					(transmit_size - 1))
				reg_mcs |= LM4_I2C_MCS_STOP;

			LM4_I2C_MCS(port) = reg_mcs;

			rv = wait_idle(port);
			if (rv)
				return rv;
		}
	}

	if (receive_size) {
		if (transmit_size)
			/* resend start bit when change direction */
			started = 0;

		LM4_I2C_MSA(port) = (slave_addr & 0xff) | 0x01;
		for (i = 0; i < receive_size; i++) {
			LM4_I2C_MDR(port) = receive_data[i];
			/* MCS receive sequence on multi-byte read:
			 *     0xb 0x9 0x9 ... 0x9 0x5
			 * Single byte read:
			 *     0x7
			 */
			reg_mcs = LM4_I2C_MCS_RUN;
			if (!started) {
				started = 1;
				reg_mcs |= LM4_I2C_MCS_START;
			}
			/* ACK all bytes except the last one */
			if (stop && i == (receive_size - 1))
				reg_mcs |= LM4_I2C_MCS_STOP;
			else
				reg_mcs |= LM4_I2C_MCS_ACK;

			LM4_I2C_MCS(port) = reg_mcs;
			rv = wait_idle(port);
			if (rv)
				return rv;
			receive_data[i] = LM4_I2C_MDR(port) & 0xff;
		}
	}

	/* Check for error conditions */
	if (LM4_I2C_MCS(port) & (LM4_I2C_MCS_CLKTO | LM4_I2C_MCS_ARBLST |
				 LM4_I2C_MCS_ERROR))
		return EC_ERROR_UNKNOWN;

	return EC_SUCCESS;
}

void i2c_lock(int port, int lock)
{
	if (lock) {
		mutex_lock(port_mutex + port);
	} else {
		mutex_unlock(port_mutex + port);
	}
}

int i2c_read16(int port, int slave_addr, int offset, int *data)
{
	int rv;
	uint8_t reg, buf[2];

	reg = offset & 0xff;
	/* I2C read 16-bit word:
	 * Transmit 8-bit offset, and read 16bits
	 */
	mutex_lock(port_mutex + port);
	rv = i2c_transmit_receive(port, slave_addr, &reg, 1, buf, 2,
					START, STOP);
	mutex_unlock(port_mutex + port);

	if (rv)
		return rv;

	if (slave_addr & I2C_FLAG_BIG_ENDIAN)
		*data = ((int)buf[0] << 8) | buf[1];
	else
		*data = ((int)buf[1] << 8) | buf[0];

	return EC_SUCCESS;
}


int i2c_write16(int port, int slave_addr, int offset, int data)
{
	int rv;
	uint8_t buf[3];

	buf[0] = offset & 0xff;

	if (slave_addr & I2C_FLAG_BIG_ENDIAN) {
		buf[1] = (data >> 8) & 0xff;
		buf[2] = data & 0xff;
	} else {
		buf[1] = data & 0xff;
		buf[2] = (data >> 8) & 0xff;
	}

	mutex_lock(port_mutex + port);
	rv = i2c_transmit_receive(port, slave_addr, buf, 3, 0, 0,
					START, STOP);
	mutex_unlock(port_mutex + port);

	return rv;
}


int i2c_read8(int port, int slave_addr, int offset, int* data)
{
	int rv;
	uint8_t reg, val;

	reg = offset;

	mutex_lock(port_mutex + port);
	rv = i2c_transmit_receive(port, slave_addr, &reg, 1, &val, 1,
					START, STOP);
	mutex_unlock(port_mutex + port);

	if (!rv)
		*data = val;

	return rv;
}


int i2c_write8(int port, int slave_addr, int offset, int data)
{
	int rv;
	uint8_t buf[2];

	buf[0] = offset;
	buf[1] = data;

	mutex_lock(port_mutex + port);
	rv = i2c_transmit_receive(port, slave_addr, buf, 2, 0, 0,
					START, STOP);
	mutex_unlock(port_mutex + port);

	return rv;
}


int i2c_read_string(int port, int slave_addr, int offset, uint8_t *data,
	int len)
{
	int rv;
	uint8_t reg, block_length;

	mutex_lock(port_mutex + port);

	reg = offset;
	/* Send device reg space offset, and read back block length.
	 * Keep this session open without a stop */
	rv = i2c_transmit_receive(port, slave_addr, &reg, 1, &block_length, 1,
				  START, NO_STOP);
	if (rv)
		goto exit;

	if (len && block_length > (len - 1))
		block_length = len - 1;

	rv = i2c_transmit_receive(port, slave_addr, 0, 0, data, block_length,
				  NO_START, STOP);
	data[block_length] = 0;

exit:
	mutex_unlock(port_mutex + port);
	return rv;
}

int get_sda_from_i2c_port(int port, enum gpio_signal *sda)
{
	int i;

	/* Find the matching port in i2c_ports[] table. */
	for (i = 0; i < I2C_PORTS_USED; i++) {
		if (i2c_ports[i].port == port)
			break;
	}

	/* Crash if the port given is not in the i2c_ports[] table. */
	ASSERT(i != I2C_PORTS_USED);

	/* Check if the SCL and SDA pins have been defined for this port. */
	if (i2c_ports[i].scl == 0 && i2c_ports[i].sda == 0)
		return EC_ERROR_INVAL;

	*sda = i2c_ports[i].sda;
	return EC_SUCCESS;
}

int get_scl_from_i2c_port(int port, enum gpio_signal *scl)
{
	int i;

	/* Find the matching port in i2c_ports[] table. */
	for (i = 0; i < I2C_PORTS_USED; i++) {
		if (i2c_ports[i].port == port)
			break;
	}

	/* Crash if the port given is not in the i2c_ports[] table. */
	ASSERT(i != I2C_PORTS_USED);

	/* Check if the SCL and SDA pins have been defined for this port. */
	if (i2c_ports[i].scl == 0 && i2c_ports[i].sda == 0)
		return EC_ERROR_INVAL;

	*scl = i2c_ports[i].scl;
	return EC_SUCCESS;
}

void i2c_raw_set_scl(int port, int level)
{
	enum gpio_signal g;

	if (get_scl_from_i2c_port(port, &g) == EC_SUCCESS)
		gpio_set_level(g, level);
}

void i2c_raw_set_sda(int port, int level)
{
	enum gpio_signal g;

	if (get_sda_from_i2c_port(port, &g) == EC_SUCCESS)
		gpio_set_level(g, level);
}

int i2c_raw_get_scl(int port)
{
	enum gpio_signal g;
	int ret;

	/* If no SCL pin defined for this port, then return 1 to appear idle. */
	if (get_scl_from_i2c_port(port, &g) != EC_SUCCESS)
		return 1;

	/* If we are driving the pin low, it must be low. */
	if (gpio_get_level(g) == 0)
		return 0;

	/*
	 * Otherwise, we need to toggle it to an input to read the true pin
	 * state.
	 */
	gpio_set_flags(g, GPIO_INPUT);
	ret = gpio_get_level(g);
	gpio_set_flags(g, GPIO_OUTPUT | GPIO_OPEN_DRAIN);

	return ret;
}

int i2c_raw_get_sda(int port)
{
	enum gpio_signal g;
	int ret;

	/* If no SDA pin defined for this port, then return 1 to appear idle. */
	if (get_sda_from_i2c_port(port, &g) != EC_SUCCESS)
		return 1;

	/* If we are driving the pin low, it must be low. */
	if (gpio_get_level(g) == 0)
		return 0;

	/*
	 * Otherwise, we need to toggle it to an input to read the true pin
	 * state.
	 */
	gpio_set_flags(g, GPIO_INPUT);
	ret = gpio_get_level(g);
	gpio_set_flags(g, GPIO_OUTPUT | GPIO_OPEN_DRAIN);

	return ret;
}

int i2c_get_line_levels(int port)
{
	/* Conveniently, MBMON bit (1 << 1) is SDA and (1 << 0) is SCL. */
	return LM4_I2C_MBMON(port) & 0x03;
}

int i2c_raw_mode(int port, int enable)
{
	enum gpio_signal sda, scl;
	static struct mutex raw_mode_mutex;

	/* Get the SDA and SCL pins for this port. If none, then return. */
	if (get_sda_from_i2c_port(port, &sda) != EC_SUCCESS)
		return EC_ERROR_INVAL;
	if (get_scl_from_i2c_port(port, &scl) != EC_SUCCESS)
		return EC_ERROR_INVAL;

	if (enable) {
		/*
		 * Lock access to raw mode functionality. Note, this is
		 * necessary because when we exit raw mode, we put all I2C
		 * ports into normal mode. This means that if another port
		 * is using the raw mode capabilities, that port will be
		 * re-configured from underneath it.
		 */
		mutex_lock(&raw_mode_mutex);

		/*
		 * To enable raw mode, take out of alternate function mode and
		 * set the flags to open drain output.
		 */
		gpio_set_alternate_function(gpio_list[sda].port,
						gpio_list[sda].mask, 0);
		gpio_set_alternate_function(gpio_list[scl].port,
						gpio_list[scl].mask, 0);

		gpio_set_flags(scl, GPIO_OUTPUT | GPIO_OPEN_DRAIN);
		gpio_set_level(scl, 1);
		gpio_set_flags(sda, GPIO_OUTPUT | GPIO_OPEN_DRAIN);
		gpio_set_level(sda, 1);
	} else {
		/*
		 * Configure the I2C pins to exit raw mode and return
		 * to normal mode.
		 */
		gpio_set_alternate_function(gpio_list[sda].port,
						gpio_list[sda].mask, 3);
		gpio_set_alternate_function(gpio_list[scl].port,
						gpio_list[scl].mask, 3);

		gpio_set_flags(scl, GPIO_OUTPUT);
		gpio_set_flags(sda, GPIO_OUTPUT | GPIO_OPEN_DRAIN);

		/* Unlock mutex, allow other I2C busses to use raw mode. */
		mutex_unlock(&raw_mode_mutex);
		return EC_SUCCESS;
	}

	return EC_SUCCESS;
}

/*
 * Unwedge the i2c bus for the given port.
 *
 * Some devices on our i2c busses keep power even if we get a reset.  That
 * means that they could be part way through a transaction and could be
 * driving the bus in a way that makes it hard for us to talk on the bus.
 * ...or they might listen to the next transaction and interpret it in a
 * weird way.
 *
 * Note that devices could be in one of several states:
 * - If a device got interrupted in a write transaction it will be watching
 *   for additional data to finish its write.  It will probably be looking to
 *   ack the data (drive the data line low) after it gets everything.
 * - If a device got interrupted while responding to a register read, it will
 *   be watching for clocks and will drive data out when it sees clocks.  At
 *   the moment it might be trying to send out a 1 (so both clock and data
 *   may be high) or it might be trying to send out a 0 (so it's driving data
 *   low).
 *
 * We attempt to unwedge the bus by doing:
 * - If SCL is being held low, then a slave is clock extending. The only
 *   thing we can do is try to wait until the slave stops clock extending.
 * - Otherwise, we will toggle the clock until the slave releases the SDA line.
 *   Once the SDA line is released, try to send a STOP bit. Rinse and repeat
 *   until either the bus is normal, or we run out of attempts.
 *
 * Note this should work for most devices, but depending on the slaves i2c
 * state machine, it may not be possible to unwedge the bus.
 */
int i2c_unwedge(int port)
{
	int i, j;
	int ret = EC_SUCCESS;

	/* Try to put port in to raw bit bang mode. */
	if (i2c_raw_mode(port, 1) != EC_SUCCESS)
		return EC_ERROR_UNKNOWN;

	/*
	 * If clock is low, wait for a while in case of clock stretched
	 * by a slave.
	 */
	if (!i2c_raw_get_scl(port)) {
		for (i = 0;; i++) {
			if (i >= UNWEDGE_SCL_ATTEMPTS) {
				/*
				 * If we get here, a slave is holding the clock
				 * low and there is nothing we can do.
				 */
				CPRINTF("I2C unwedge failed, SCL is being held low\n");
				ret = EC_ERROR_UNKNOWN;
				goto unwedge_done;
			}
			udelay(I2C_BITBANG_DELAY_US);
			if (i2c_raw_get_scl(port))
				break;
		}
	}

	if (i2c_raw_get_sda(port))
		goto unwedge_done;

	CPRINTF("I2C unwedge called with SDA held low\n");

	/* Keep trying to unwedge the SDA line until we run out of attempts. */
	for (i = 0; i < UNWEDGE_SDA_ATTEMPTS; i++) {
		/* Drive the clock high. */
		i2c_raw_set_scl(port, 1);
		udelay(I2C_BITBANG_DELAY_US);

		/*
		 * Clock through the problem by clocking out 9 bits. If slave
		 * releases the SDA line, then we can stop clocking bits and
		 * send a STOP.
		 */
		for (j = 0; j < 9; j++) {
			if (i2c_raw_get_sda(port))
				break;

			i2c_raw_set_scl(port, 0);
			udelay(I2C_BITBANG_DELAY_US);
			i2c_raw_set_scl(port, 1);
			udelay(I2C_BITBANG_DELAY_US);
		}

		/* Take control of SDA line and issue a STOP command. */
		i2c_raw_set_sda(port, 0);
		udelay(I2C_BITBANG_DELAY_US);
		i2c_raw_set_sda(port, 1);
		udelay(I2C_BITBANG_DELAY_US);

		/* Check if the bus is unwedged. */
		if (i2c_raw_get_sda(port) && i2c_raw_get_scl(port))
			break;
	}

	if (!i2c_raw_get_sda(port)) {
		CPRINTF("I2C unwedge failed, SDA still low\n");
		ret = EC_ERROR_UNKNOWN;
	}
	if (!i2c_raw_get_scl(port)) {
		CPRINTF("I2C unwedge failed, SCL still low\n");
		ret = EC_ERROR_UNKNOWN;
	}

unwedge_done:
	/* Take port out of raw bit bang mode. */
	i2c_raw_mode(port, 0);

	return ret;
}

static int i2c_freq_changed(void)
{
	int freq = clock_get_freq();
	int i;

	for (i = 0; i < I2C_PORTS_USED; i++) {
		/* From datasheet:
		 *     SCL_PRD = 2 * (1 + TPR) * (SCL_LP + SCL_HP) * CLK_PRD
		 *
		 * so:
		 *     TPR = SCL_PRD / (2 * (SCL_LP + SCL_HP) * CLK_PRD) - 1
		 *
		 * converting from period to frequency:
		 *     TPR = CLK_FREQ / (SCL_FREQ * 2 * (SCL_LP + SCL_HP)) - 1
		 */
		const int d = 2 * (6 + 4) * (i2c_ports[i].kbps * 1000);

		/* Round TPR up, so desired kbps is an upper bound */
		const int tpr = (freq + d - 1) / d - 1;

#ifdef PRINT_I2C_SPEEDS
		const int f = freq / (2 * (1 + tpr) * (6 + 4));
		CPRINTF("[%T I2C%d clk=%d tpr=%d freq=%d]\n",
			i2c_ports[i].port, freq, tpr, f);
#endif

		LM4_I2C_MTPR(i2c_ports[i].port) = tpr;
	}

	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_FREQ_CHANGE, i2c_freq_changed, HOOK_PRIO_DEFAULT + 1);

/*****************************************************************************/
/* Interrupt handlers */

/* Handles an interrupt on the specified port. */
static void handle_interrupt(int port)
{
	int id = task_waiting_on_port[port];

	/* Clear the interrupt status */
	LM4_I2C_MICR(port) = LM4_I2C_MMIS(port);

	/* Wake up the task which was waiting on the I2C interrupt, if any. */
	if (id != TASK_ID_INVALID)
		task_set_event(id, TASK_EVENT_I2C_IDLE, 0);
}


static void i2c0_interrupt(void) { handle_interrupt(0); }
static void i2c1_interrupt(void) { handle_interrupt(1); }
static void i2c2_interrupt(void) { handle_interrupt(2); }
static void i2c3_interrupt(void) { handle_interrupt(3); }
static void i2c4_interrupt(void) { handle_interrupt(4); }
static void i2c5_interrupt(void) { handle_interrupt(5); }

DECLARE_IRQ(LM4_IRQ_I2C0, i2c0_interrupt, 2);
DECLARE_IRQ(LM4_IRQ_I2C1, i2c1_interrupt, 2);
DECLARE_IRQ(LM4_IRQ_I2C2, i2c2_interrupt, 2);
DECLARE_IRQ(LM4_IRQ_I2C3, i2c3_interrupt, 2);
DECLARE_IRQ(LM4_IRQ_I2C4, i2c4_interrupt, 2);
DECLARE_IRQ(LM4_IRQ_I2C5, i2c5_interrupt, 2);

/*****************************************************************************/
/* Console commands */

static void scan_bus(int port, const char *desc)
{
	int rv;
	int a;

	ccprintf("Scanning %d %s", port, desc);

	/* Don't scan a busy port, since reads will just fail / time out */
	a = LM4_I2C_MBMON(port);
	if ((a & 0x03) != 0x03) {
		ccprintf(": port busy (SDA=%d, SCL=%d)\n",
		 (LM4_I2C_MBMON(port) & 0x02) ? 1 : 0,
		 (LM4_I2C_MBMON(port) & 0x01) ? 1 : 0);
		return;
	}

	mutex_lock(port_mutex + port);

	for (a = 0; a < 0x100; a += 2) {
		ccputs(".");

		/* Do a single read */
		LM4_I2C_MSA(port) = a | 0x01;
		LM4_I2C_MCS(port) = 0x07;
		rv = wait_idle(port);
		if (rv == EC_SUCCESS)
			ccprintf("\n  0x%02x", a);
	}

	mutex_unlock(port_mutex + port);
	ccputs("\n");
}


static int command_i2cread(int argc, char **argv)
{
	int port, addr, count = 1;
	char *e;
	int rv;
	int d, i;

	if (argc < 3)
		return EC_ERROR_PARAM_COUNT;

	port = strtoi(argv[1], &e, 0);
	if (*e)
		return EC_ERROR_PARAM1;

	for (i = 0; i < I2C_PORTS_USED && port != i2c_ports[i].port; i++)
		;
	if (i >= I2C_PORTS_USED)
		return EC_ERROR_PARAM1;

	addr = strtoi(argv[2], &e, 0);
	if (*e || (addr & 0x01))
		return EC_ERROR_PARAM2;

	if (argc > 3) {
		count = strtoi(argv[3], &e, 0);
		if (*e)
			return EC_ERROR_PARAM3;
	}

	ccprintf("Reading %d bytes from %d:0x%02x:", count, port, addr);
	mutex_lock(port_mutex + port);
	LM4_I2C_MSA(port) = addr | 0x01;
	for (i = 0; i < count; i++) {
		if (i == 0)
			LM4_I2C_MCS(port) = (count > 1 ? 0x0b : 0x07);
		else
			LM4_I2C_MCS(port) = (i == count - 1 ? 0x05 : 0x09);
		rv = wait_idle(port);
		if (rv != EC_SUCCESS) {
			mutex_unlock(port_mutex + port);
			return rv;
		}
		d = LM4_I2C_MDR(port) & 0xff;
		ccprintf(" 0x%02x", d);
	}
	mutex_unlock(port_mutex + port);
	ccputs("\n");
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(i2cread, command_i2cread,
			"port addr [count]",
			"Read from I2C",
			NULL);


static int command_scan(int argc, char **argv)
{
	int i;

	for (i = 0; i < I2C_PORTS_USED; i++)
		scan_bus(i2c_ports[i].port, i2c_ports[i].name);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(i2cscan, command_scan,
			NULL,
			"Scan I2C ports for devices",
			NULL);


/*****************************************************************************/
/* Initialization */

/* Configures GPIOs for the module. */
static void configure_gpio(void)
{
#ifdef BOARD_link
	/* PA6:7 = I2C1 SCL/SDA; PB2:3 = I2C0 SCL/SDA; PB6:7 = I2C5 SCL/SDA */
	gpio_set_alternate_function(LM4_GPIO_A, 0xc0, 3);
	gpio_set_alternate_function(LM4_GPIO_B, 0xcc, 3);

	/* Configure SDA as open-drain.  SCL should not be open-drain,
	 * since it has an internal pull-up. */
	LM4_GPIO_ODR(LM4_GPIO_A) |= 0x80;
	LM4_GPIO_ODR(LM4_GPIO_B) |= 0x88;
#else
	/* PG6:7 = I2C5 SCL/SDA */
	gpio_set_alternate_function(LM4_GPIO_G, 0xc0, 3);

	/* Configure SDA as open-drain.  SCL should not be open-drain,
	 * since it has an internal pull-up. */
	LM4_GPIO_ODR(LM4_GPIO_G) |= 0x80;
#endif
}


static int i2c_init(void)
{
	volatile uint32_t scratch  __attribute__((unused));
	uint32_t mask = 0;
	int i;

	/* Enable I2C modules and delay a few clocks */
	for (i = 0; i < I2C_PORTS_USED; i++)
		mask |= 1 << i2c_ports[i].port;

	LM4_SYSTEM_RCGCI2C |= mask;
	scratch = LM4_SYSTEM_RCGCI2C;

	/* Configure GPIOs */
	configure_gpio();

	/* No tasks are waiting on ports */
	for (i = 0; i < NUM_PORTS; i++)
		task_waiting_on_port[i] = TASK_ID_INVALID;

	/* Initialize ports as master, with interrupts enabled */
	for (i = 0; i < I2C_PORTS_USED; i++)
		LM4_I2C_MCR(i2c_ports[i].port) = 0x10;

	/* Set initial clock frequency */
	i2c_freq_changed();

	/* Enable irqs */
	task_enable_irq(LM4_IRQ_I2C0);
	task_enable_irq(LM4_IRQ_I2C1);
	task_enable_irq(LM4_IRQ_I2C2);
	task_enable_irq(LM4_IRQ_I2C3);
	task_enable_irq(LM4_IRQ_I2C4);
	task_enable_irq(LM4_IRQ_I2C5);

	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_INIT, i2c_init, HOOK_PRIO_DEFAULT);
