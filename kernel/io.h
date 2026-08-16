/* io.h — port I/O primitives.
 *
 * x86 has a separate address space for device registers, reached with the in
 * and out instructions rather than ordinary memory access. Every hardware
 * driver in this kernel bottoms out here.
 */
#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Short delay used between writes to slow legacy devices (notably the PIC).
 * Writing to unused port 0x80 takes roughly a bus cycle and has no effect. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif /* IO_H */
