/* isr.h — interrupt and exception dispatch.
 *
 * Every interrupt enters through an assembly stub in isr.asm which saves the
 * full CPU state, then calls into C with a pointer to that saved state.
 * Modules register handlers by vector number; nothing else touches the IDT.
 */
#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/* The saved CPU state, laid out to match exactly what isr.asm pushes.
 * Field order here is the reverse of push order, because the stack grows
 * down: the last thing pushed is at the lowest address, i.e. first here. */
typedef struct {
    uint32_t ds;                                     /* pushed by our stub */
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* pusha */
    uint32_t int_no, err_code;                       /* pushed by our stub */
    uint32_t eip, cs, eflags, useresp, ss;           /* pushed by the CPU */
} registers_t;

typedef void (*isr_handler_t)(registers_t *regs);

/* Hardware IRQ 0-15 arrive on vectors 32-47 after the PIC is remapped. */
#define IRQ_BASE     32
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

void isr_init(void);

/* Register a handler for a CPU exception (0-31) or an IRQ vector (32-47).
 * Use IRQ_BASE + IRQ_TIMER etc. for hardware interrupts. */
void isr_register(uint8_t vector, isr_handler_t handler);

/* Enable / disable interrupts globally. */
static inline void sti(void) { __asm__ volatile ("sti"); }
static inline void cli(void) { __asm__ volatile ("cli"); }

/* True if the interrupt flag is currently set. */
static inline int interrupts_enabled(void)
{
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

#endif /* ISR_H */
