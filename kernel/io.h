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

/* 32-bit port access. Needed for PCI configuration space, which is addressed
 * a dword at a time through a pair of ports. */
static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Short delay used between writes to slow legacy devices (notably the PIC).
 * Writing to unused port 0x80 takes roughly a bus cycle and has no effect. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* Block transfers. ATA PIO moves a sector as 256 words; doing that with a
 * loop of inw/outw works, but the string instructions are what the
 * controller is paced for and keep the wait-for-DRQ window short. */
static inline void insw(uint16_t port, void *addr, uint32_t count)
{
    __asm__ volatile ("rep insw"
                      : "+D"(addr), "+c"(count)
                      : "d"(port)
                      : "memory");
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count)
{
    const void *ptr = addr;
    __asm__ volatile ("rep outsw"
                      : "+S"(ptr), "+c"(count)
                      : "d"(port)
                      : "memory");
}

#endif /* IO_H */
