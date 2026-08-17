#include "serial.h"
#include "io.h"

#include <stdbool.h>

#define COM1 0x3F8

/* Register offsets from the base port. Several are context-dependent because
 * the UART multiplexes them behind the DLAB bit in the line control register. */
#define REG_DATA        0   /* read/write data, or baud divisor low when DLAB=1 */
#define REG_INT_ENABLE  1   /* interrupt enable, or baud divisor high when DLAB=1 */
#define REG_FIFO_CTRL   2
#define REG_LINE_CTRL   3
#define REG_MODEM_CTRL  4
#define REG_LINE_STATUS 5

#define LSR_THR_EMPTY  0x20 /* transmit holding register is ready for a byte */

#define MCR_LOOPBACK   0x1E /* loopback mode with OUT1/OUT2/RTS asserted */
#define MCR_NORMAL     0x0B /* DTR + RTS + OUT2 */

/* Whether a UART actually answered at COM1. Not every machine has a serial
 * port: VMware gives a VM none unless one is configured, and most modern PCs
 * have no physical port at all. */
static bool present;

/* Bound on the transmit wait.
 *
 * Even with a UART present this loop must terminate. If the port is absent the
 * status register reads back whatever the bus floats to -- commonly 0xFF, which
 * happens to look "ready", but 0x00 on some chipsets, which looks permanently
 * busy. An unbounded spin there hangs the kernel inside the very first kprintf,
 * before anything has reached the screen: a black display with no diagnostic,
 * which is the hardest possible failure to work backwards from.
 *
 * At 38400 baud a character takes roughly 260us to shift out, so this is
 * several orders of magnitude more headroom than a working port ever needs. */
#define TX_SPIN_LIMIT 100000

void serial_init(void)
{
    /* Probe before trusting the port. Putting the UART in loopback mode wires
     * its transmitter to its own receiver: write a byte, and a real chip hands
     * the same byte straight back. Empty sockets and floating buses do not. */
    outb(COM1 + REG_INT_ENABLE, 0x00);
    outb(COM1 + REG_LINE_CTRL,  0x80);  /* DLAB=1: next two regs are the divisor */
    outb(COM1 + REG_DATA,       0x03);  /* 115200 / 3 = 38400 baud */
    outb(COM1 + REG_INT_ENABLE, 0x00);
    outb(COM1 + REG_LINE_CTRL,  0x03);  /* DLAB=0, 8 bits, no parity, 1 stop */
    outb(COM1 + REG_FIFO_CTRL,  0xC7);  /* enable FIFO, clear it */

    outb(COM1 + REG_MODEM_CTRL, MCR_LOOPBACK);
    outb(COM1 + REG_DATA, 0xAE);

    present = (inb(COM1 + REG_DATA) == 0xAE);

    /* Leave loopback regardless, so a port that failed the probe is not left
     * in a strange state for anything that looks at it later. */
    outb(COM1 + REG_MODEM_CTRL, MCR_NORMAL);
}

bool serial_present(void)
{
    return present;
}

void serial_putc(char c)
{
    if (!present)
        return;

    /* Polling rather than interrupt-driven: the debug channel has to keep
     * working when interrupts are exactly what is broken. */
    for (uint32_t spins = 0; spins < TX_SPIN_LIMIT; spins++) {
        if (inb(COM1 + REG_LINE_STATUS) & LSR_THR_EMPTY) {
            outb(COM1 + REG_DATA, (uint8_t)c);
            return;
        }
    }

    /* Gave up. Dropping debug output is survivable; hanging the kernel to
     * deliver it is not. */
}
