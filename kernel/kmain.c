/* kmain.c — the kernel's C entry point.
 *
 * boot.asm hands control here with a stack established and GRUB's boot
 * information available. Subsystems are brought up in dependency order.
 */
#include "console.h"
#include "vga.h"

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

void kmain(uint32_t magic, uint32_t *mb_info)
{
    console_init();

    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("MyOS v0.1 - bare metal x86 (i386, protected mode)\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    /* GRUB leaves this exact value in eax to prove it was the loader and that
     * the boot info pointer is valid. Anything else means we were started by
     * something that does not implement multiboot. */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic: expected %08x, got %08x",
              MULTIBOOT_BOOTLOADER_MAGIC, magic);

    kprintf("multiboot magic  : %08x  OK\n", magic);
    kprintf("multiboot info   : %p\n", (void *)mb_info);

    /* Field 0 of the info struct is a flags word; bit 0 means the memory
     * size fields that follow are valid. */
    if (mb_info && (mb_info[0] & 0x1)) {
        uint32_t lower_kb = mb_info[1];
        uint32_t upper_kb = mb_info[2];
        kprintf("memory           : %u KB low, %u KB high (%u MB total)\n",
                lower_kb, upper_kb, (lower_kb + upper_kb) / 1024);
    }

    kprintf("\nboot pipeline verified. halting.\n");

    for (;;)
        __asm__ volatile ("hlt");
}
