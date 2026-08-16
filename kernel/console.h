/* console.h — the kernel's only output interface.
 *
 * Every other module prints through kprintf. Console fans output to both the
 * VGA screen (what a user sees) and the serial port (what survives a crash),
 * so no module needs to know or care which channels exist.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);

/* Supported conversions: %c %s %d %i %u %x %X %p %% and %% with a zero-padded
 * width of 1-8 for the integer forms, e.g. %08x. Deliberately minimal. */
void kprintf(const char *fmt, ...);

void kputc(char c);
void kputs(const char *s);

/* Return the cursor to the top-left on both channels, so a repeated redraw
 * overwrites in place rather than scrolling. The VGA side moves the hardware
 * cursor directly; the serial side emits the equivalent ANSI escape, since a
 * terminal is the only thing on that end that understands one. */
void console_home(void);

/* Print a message, then halt the CPU permanently. Never returns. */
void panic(const char *fmt, ...) __attribute__((noreturn));

#endif /* CONSOLE_H */
