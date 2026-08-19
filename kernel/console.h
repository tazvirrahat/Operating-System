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

/* The column the next character will land in.
 *
 * Counted by the console itself rather than read back from a device. The
 * caller that needs this is padding a line out to a fixed width, and it used
 * to ask the VGA driver -- which was right only while output actually went to
 * the VGA text buffer. Once the framebuffer console took over, the text-mode
 * cursor stopped moving and the answer stopped changing.
 *
 * Wrapping is not modelled: the width differs between the text console, the
 * framebuffer console and a terminal window, and the only caller emits lines
 * far shorter than any of them. A line that does wrap will read high until the
 * next newline resets it.
 */
int console_column(void);

/* Return the cursor to the top-left on both channels, so a repeated redraw
 * overwrites in place rather than scrolling. The VGA side moves the hardware
 * cursor directly; the serial side emits the equivalent ANSI escape, since a
 * terminal is the only thing on that end that understands one. */
void console_home(void);

/* Redirect character output.
 *
 * With a sink installed, kputc sends characters there instead of to the VGA
 * text buffer — which does not exist while the display is in graphics mode.
 * This is how the GUI's terminal window receives shell output without the
 * shell knowing anything about windows. Serial output continues regardless,
 * so the transcript is unbroken across the switch.
 *
 * Pass NULL to restore normal text-mode output.
 */
typedef void (*console_sink_t)(char c);
void console_set_sink(console_sink_t sink);

/* Print a message, then halt the CPU permanently. Never returns. */
void panic(const char *fmt, ...) __attribute__((noreturn));

#endif /* CONSOLE_H */
