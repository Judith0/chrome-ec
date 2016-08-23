/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Debug console for Chrome EC */

#ifndef __CROS_EC_CONSOLE_H
#define __CROS_EC_CONSOLE_H

#include "common.h"

/* Console command; used by DECLARE_CONSOLE_COMMAND macro. */
struct console_command {
	/* Command name.  Case-insensitive. */
	const char *name;
	/* Handler for the command.  argv[0] will be the command name. */
	int (*handler)(int argc, char **argv);
#ifdef CONFIG_CONSOLE_CMDHELP
	/* Description of args */
	const char *argdesc;
	/* Short help for command */
	const char *shorthelp;
#endif
};

/* Console channels */
enum console_channel {
	#define CONSOLE_CHANNEL(enumeration, string) enumeration,
	#include "include/console_channel.inc"
	#undef CONSOLE_CHANNEL

	/* Channel count; not itself a channel */
	CC_CHANNEL_COUNT
};

/* Mask in channel_mask for a particular channel */
#define CC_MASK(channel)	(1UL << (channel))

/* Mask to use to enable all channels */
#define CC_ALL			0xffffffffUL

/**
 * Put a string to the console channel.
 *
 * @param channel	Output chanel
 * @param outstr	String to write
 *
 * @return non-zero if output was truncated.
 */
int cputs(enum console_channel channel, const char *outstr);

/**
 * Print formatted output to the console channel.
 *
 * @param channel	Output chanel
 * @param format	Format string; see printf.h for valid formatting codes
 *
 * @return non-zero if output was truncated.
 */
int cprintf(enum console_channel channel, const char *format, ...);

/**
 * Print formatted output with timestamp. This is like:
 *   cprintf(channel, "[%T " + format + "]\n", ...)
 *
 * @param channel	Output channel
 * @param format	Format string; see printf.h for valid formatting codes
 *
 * @return non-zero if output was truncated.
 */
int cprints(enum console_channel channel, const char *format, ...);

/**
 * Flush the console output for all channels.
 */
void cflush(void);

/* Convenience macros for printing to the command channel.
 *
 * Modules may define similar macros in their .c files for their own use; it is
 * recommended those module-specific macros be named CPUTS and CPRINTF. */
#define ccputs(outstr) cputs(CC_COMMAND, outstr)
/* gcc allows variable arg lists in macros; see
 * http://gcc.gnu.org/onlinedocs/gcc/Variadic-Macros.html */
#define ccprintf(format, args...) cprintf(CC_COMMAND, format, ## args)
#define ccprints(format, args...) cprints(CC_COMMAND, format, ## args)

/**
 * Called by UART when a line of input is pending.
 */
void console_has_input(void);

/**
 * Register a console command handler.
 *
 * @param name		Command name; must not be the beginning of another
 *			existing command name.  Note this is NOT in quotes
 *		        so it can be concatenated to form a struct name.
 * @param routine	Command handling routine, of the form
 *			int handler(int argc, char **argv)
 * @param argdesc	String describing arguments to command; NULL if none.
 * @param shorthelp	String with one-line description of command.
 */
#ifndef HAS_TASK_CONSOLE
#define DECLARE_CONSOLE_COMMAND(NAME, ROUTINE, ARGDESC, SHORTHELP)	\
	int (ROUTINE)(int argc, char **argv) __attribute__((unused))
#elif defined(CONFIG_CONSOLE_CMDHELP)
#define DECLARE_CONSOLE_COMMAND(NAME, ROUTINE, ARGDESC, SHORTHELP)	\
	static const char __con_cmd_label_##NAME[] = #NAME;		\
	struct size_check##NAME {					\
		int field[2 * (sizeof(__con_cmd_label_##NAME) < 16) - 1]; }; \
	const struct console_command __keep __con_cmd_##NAME		\
	__attribute__((section(".rodata.cmds." #NAME)))	=		\
	{ .name = __con_cmd_label_##NAME,				\
	  .handler = ROUTINE,						\
	  .argdesc = ARGDESC,						\
	  .shorthelp = SHORTHELP					\
	}
#else
#define DECLARE_CONSOLE_COMMAND(NAME, ROUTINE, ARGDESC, SHORTHELP)	\
	static const char __con_cmd_label_##NAME[] = #NAME;		\
	struct size_check##NAME {					\
		int field[2 * (sizeof(__con_cmd_label_##NAME) < 16) - 1]; }; \
	const struct console_command __keep __con_cmd_##NAME		\
	__attribute__((section(".rodata.cmds." #NAME))) =		\
	{ .name = __con_cmd_label_##NAME,				\
	  .handler = ROUTINE,						\
	}
#endif

#endif  /* __CROS_EC_CONSOLE_H */
