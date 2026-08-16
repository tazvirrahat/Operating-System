/* idt.h — Interrupt Descriptor Table.
 *
 * 256 entries telling the CPU which code to run for each interrupt vector.
 * Until a valid IDT is loaded, any fault is unrecoverable: the CPU has nowhere
 * to go, escalates to a double then triple fault, and resets the machine.
 * That silent reboot loop is the classic early-x86 failure mode.
 */
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

/* Gate type + attributes.
 *   0x8E = present, DPL 0, 32-bit interrupt gate (clears IF on entry)
 *   0xEE = same but DPL 3, so ring 3 code is allowed to invoke it.
 *          Only the syscall vector uses this; everything else must be
 *          unreachable from user mode. */
#define IDT_GATE_KERNEL 0x8E
#define IDT_GATE_USER   0xEE

void idt_init(void);
void idt_set_gate(uint8_t vector, uint32_t handler, uint8_t flags);

#endif /* IDT_H */
