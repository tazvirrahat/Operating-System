#include "idt.h"
#include "gdt.h"
#include "console.h"

/* Like GDT entries, the handler address is split across non-adjacent fields
 * for historical reasons. Getting this split wrong sends the CPU to a garbage
 * address on the first interrupt, which triple-faults. */
struct idt_entry {
    uint16_t offset_low;    /* handler address bits 0-15 */
    uint16_t selector;      /* code segment selector to run the handler in */
    uint8_t  zero;          /* always zero */
    uint8_t  type_attr;
    uint16_t offset_high;   /* handler address bits 16-31 */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idt_ptr;

/* The assembly stubs, one per vector. Declared as functions purely so we can
 * take their addresses; they are never called from C. */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

void idt_set_gate(uint8_t vector, uint32_t handler, uint8_t flags)
{
    idt[vector].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].zero        = 0;
    idt[vector].type_attr   = flags;
}

void idt_init(void)
{
    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base  = (uint32_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].offset_high = 0;
        idt[i].selector    = 0;
        idt[i].zero        = 0;
        idt[i].type_attr   = 0;    /* not present: an unexpected vector faults */
    }

    static void (*const exception_stubs[32])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    };

    static void (*const irq_stubs[16])(void) = {
        irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15,
    };

    for (int i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, (uint32_t)exception_stubs[i], IDT_GATE_KERNEL);

    /* IRQs land on 32-47 because the PIC is remapped there. */
    for (int i = 0; i < 16; i++)
        idt_set_gate((uint8_t)(32 + i), (uint32_t)irq_stubs[i], IDT_GATE_KERNEL);

    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));

    kprintf("idt              : 256 entries, 32 exceptions + 16 irqs installed\n");
}
