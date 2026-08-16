/* gdt.h — Global Descriptor Table.
 *
 * The GDT tells the CPU how memory is divided into segments and what
 * privilege each requires. GRUB installs a temporary one to get us into
 * protected mode; we replace it with our own because GRUB's may be
 * overwritten at any time, and because we need user-mode descriptors later.
 *
 * We use a flat model: every segment covers the entire 4 GB address space.
 * Segmentation is effectively disabled; the descriptors exist only because
 * x86 requires them, and because the privilege level lives here.
 */
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Selectors are byte offsets into the GDT, which is why they step by 8. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

void gdt_init(void);

/* Sets the kernel stack the CPU switches to when an interrupt arrives while
 * running in ring 3. Used once user mode exists. */
void tss_set_kernel_stack(uint32_t esp0);

#endif /* GDT_H */
