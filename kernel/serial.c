#include "serial.h"
#include "io.h"

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

void serial_init(void)
{
    outb(COM1 + REG_INT_ENABLE, 0x00);  /* no interrupts; we poll */

    /* Set the baud rate. DLAB=1 remaps the first two registers to the divisor.
     * Divisor 3 gives 115200/3 = 38400 baud. */
    outb(COM1 + REG_LINE_CTRL,  0x80);
    outb(COM1 + REG_DATA,       0x03);
    outb(COM1 + REG_INT_ENABLE, 0x00);

    outb(COM1 + REG_LINE_CTRL,  0x03);  /* DLAB=0, 8 bits, no parity, 1 stop */
    outb(COM1 + REG_FIFO_CTRL,  0xC7);  /* enable FIFO, clear it, 14-byte threshold */
    outb(COM1 + REG_MODEM_CTRL, 0x0B);  /* DTR + RTS + OUT2 */
}

void serial_putc(char c)
{
    /* Spin until the UART has room. This is polling, which is fine for a debug
     * channel — it must work even when interrupts are broken. */
    while ((inb(COM1 + REG_LINE_STATUS) & LSR_THR_EMPTY) == 0)
        ;

    outb(COM1 + REG_DATA, (uint8_t)c);
}
