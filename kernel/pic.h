/* pic.h — 8259 Programmable Interrupt Controller.
 *
 * Two cascaded chips route the 16 hardware IRQ lines to CPU interrupt vectors.
 * By default they deliver IRQ 0-7 on vectors 8-15, which collide with the CPU's
 * own exception vectors: IRQ 0 (the timer) arrives on vector 0, the same vector
 * as divide-by-zero, making the two indistinguishable.
 *
 * Remapping to 32-47 is therefore mandatory, not an optimisation. It is also
 * x86-specific, so tutorials written for other architectures never mention it.
 */
#ifndef PIC_H
#define PIC_H

#include <stdint.h>

void pic_init(void);

/* Signal end-of-interrupt. Until this is sent the PIC considers the interrupt
 * still in service and will deliver no further ones — the reason a timer that
 * "fires exactly once" is such a common bug. */
void pic_eoi(uint8_t irq);

void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);

#endif /* PIC_H */
