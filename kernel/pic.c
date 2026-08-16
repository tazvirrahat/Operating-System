#include "pic.h"
#include "io.h"
#include "console.h"

#define PIC1_CMD  0x20      /* master */
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0      /* slave */
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

#define PIC_EOI   0x20

#define PIC1_OFFSET 32      /* IRQ 0-7  -> vectors 32-39 */
#define PIC2_OFFSET 40      /* IRQ 8-15 -> vectors 40-47 */

void pic_init(void)
{
    /* Save the current masks; the initialisation sequence clears them. */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: begin initialisation, expect ICW4. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();

    /* ICW2: the vector offset each chip should deliver on. This is the remap. */
    outb(PIC1_DATA, PIC1_OFFSET);           io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);           io_wait();

    /* ICW3: wiring between the two chips. The slave is attached to the
     * master's IRQ 2 line, expressed as a bitmask on the master and as a
     * plain identity number on the slave. */
    outb(PIC1_DATA, 0x04);                  io_wait();
    outb(PIC2_DATA, 0x02);                  io_wait();

    /* ICW4: 8086 mode rather than the ancient 8080 mode. */
    outb(PIC1_DATA, ICW4_8086);             io_wait();
    outb(PIC2_DATA, ICW4_8086);             io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);

    kprintf("pic              : remapped irq 0-15 to vectors %d-%d\n",
            PIC1_OFFSET, PIC2_OFFSET + 7);
}

void pic_eoi(uint8_t irq)
{
    /* An IRQ handled by the slave must be acknowledged on both chips: the
     * slave raised it, but the master relayed it. Acknowledging only one
     * leaves the other blocked. */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);

    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1 << (irq & 7));

    outb(port, (uint8_t)(inb(port) | bit));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1 << (irq & 7));

    outb(port, (uint8_t)(inb(port) & ~bit));
}
