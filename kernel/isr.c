#include "isr.h"
#include "idt.h"
#include "pic.h"
#include "console.h"

static isr_handler_t handlers[IDT_ENTRIES];

static const char *exception_names[32] = {
    "divide by zero",           "debug",
    "non-maskable interrupt",   "breakpoint",
    "overflow",                 "bound range exceeded",
    "invalid opcode",           "device not available",
    "double fault",             "coprocessor segment overrun",
    "invalid TSS",              "segment not present",
    "stack segment fault",      "general protection fault",
    "page fault",               "reserved",
    "x87 floating point",       "alignment check",
    "machine check",            "SIMD floating point",
    "virtualisation",           "control protection",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved",
    "hypervisor injection",     "VMM communication",
    "security exception",       "reserved",
};

void isr_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++)
        handlers[i] = 0;
}

void isr_register(uint8_t vector, isr_handler_t handler)
{
    handlers[vector] = handler;
}

/* Read CR2, which the CPU fills with the faulting address on a page fault.
 * Printing it is how we prove a fault is real: we never assigned that value. */
static uint32_t read_cr2(void)
{
    uint32_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void dump_registers(registers_t *regs)
{
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
            regs->eax, regs->ebx, regs->ecx, regs->edx);
    kprintf("  esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            regs->esi, regs->edi, regs->ebp, regs->esp_dummy);
    kprintf("  eip=%08x cs=%04x eflags=%08x\n",
            regs->eip, regs->cs & 0xFFFF, regs->eflags);
}

/* Called from isr_common in isr.asm for vectors 0-31. */
void isr_handler(registers_t *regs)
{
    uint32_t n = regs->int_no;

    if (n < 32 && handlers[n]) {
        handlers[n](regs);
        return;
    }

    /* No handler registered. Report as much as possible before stopping —
     * this output is the only evidence that will exist. */
    const char *name = (n < 32) ? exception_names[n] : "unknown";

    kprintf("\n*** CPU EXCEPTION %u: %s ***\n", n, name);
    kprintf("  error code: %08x\n", regs->err_code);

    if (n == 14) {
        uint32_t cr2 = read_cr2();
        /* Error code bits: 0=protection violation (else not-present),
         * 1=write (else read), 2=user mode (else kernel). */
        kprintf("  cr2 (faulting address): %08x\n", cr2);
        kprintf("  cause: %s, %s, %s mode\n",
                (regs->err_code & 0x1) ? "protection violation" : "page not present",
                (regs->err_code & 0x2) ? "write" : "read",
                (regs->err_code & 0x4) ? "user" : "kernel");
    }

    dump_registers(regs);

    panic("unhandled exception %u (%s) in kernel context", n, name);
}

/* Called from irq_common in isr.asm for vectors 32-47. */
void irq_handler(registers_t *regs)
{
    uint8_t irq = (uint8_t)(regs->int_no - IRQ_BASE);

    if (handlers[regs->int_no])
        handlers[regs->int_no](regs);

    /* Always acknowledge, even with no handler installed. Skipping this
     * silently stops all further interrupts on that line. */
    pic_eoi(irq);
}
