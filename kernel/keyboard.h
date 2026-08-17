/* keyboard.h — PS/2 keyboard driver.
 *
 * Interrupt-driven: IRQ 1 fires on every key event, the handler translates the
 * raw scancode and drops a character into a ring buffer. Consumers read from
 * the buffer. The driver knows nothing about the shell or any other consumer,
 * which keeps producer and consumer fully decoupled.
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>

/* Keys that have no ASCII representation are reported as control codes that
 * a keyboard cannot otherwise produce, so consumers can switch on them
 * alongside ordinary characters without widening the return type. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14

void kbd_init(void);

/* Non-blocking: returns 0 if no key is waiting. */
char kbd_poll(void);

/* Blocking: sleeps the CPU until a key arrives. */
char kbd_getchar(void);

/* True if at least one character is buffered. */
bool kbd_available(void);

/* Number of IRQ 1 interrupts seen since boot, for the kernel monitor. */
unsigned kbd_irq_count(void);

#endif /* KEYBOARD_H */
