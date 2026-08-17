/* serial.h — COM1 serial port driver.
 *
 * This is the kernel's debug channel. QEMU can pipe it to a file or to stdout,
 * so output survives a crash and reboot — which VGA output does not. It is
 * initialised first in kmain for exactly that reason.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>

void serial_init(void);
void serial_putc(char c);

/* False when no UART answered the probe at COM1, in which case serial_putc
 * discards output instead of waiting for hardware that is not there. */
bool serial_present(void);

#endif /* SERIAL_H */
