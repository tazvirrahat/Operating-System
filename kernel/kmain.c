/* kmain.c — the kernel's C entry point.
 *
 * boot.asm hands control here with a stack established and GRUB's boot
 * information available. Subsystems are brought up in dependency order:
 * output first (so failures are visible), then descriptor tables, then
 * interrupt routing, then devices.
 */
#include "console.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "pit.h"

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

void kmain(uint32_t magic, uint32_t *mb_info)
{
    /* Output before anything else. Every failure after this point is only
     * diagnosable because these two channels already work. */
    console_init();

    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("MyOS v0.2 - bare metal x86 (i386, protected mode)\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    /* GRUB leaves this exact value in eax to prove it was the loader and that
     * the boot info pointer is valid. */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic: expected %08x, got %08x",
              MULTIBOOT_BOOTLOADER_MAGIC, magic);

    kprintf("multiboot        : %08x OK\n", magic);

    if (mb_info && (mb_info[0] & 0x1)) {
        uint32_t lower_kb = mb_info[1];
        uint32_t upper_kb = mb_info[2];
        kprintf("memory           : %u KB low, %u KB high (%u MB total)\n",
                lower_kb, upper_kb, (lower_kb + upper_kb) / 1024);
    }

    gdt_init();     /* our own segments, replacing GRUB's temporary ones */
    isr_init();     /* clear the handler table before the IDT can dispatch */
    idt_init();     /* from here, a fault prints a diagnostic instead of resetting */
    pic_init();     /* mandatory remap: IRQ 0 would otherwise arrive on vector 0 */
    pit_init(100);  /* 100 Hz heartbeat */

    sti();          /* interrupts live */
    kprintf("interrupts       : enabled\n\n");

    /* Verify the timer is genuinely firing. The tick counter is incremented
     * only from the IRQ 0 handler, so if it advances while we sit in hlt,
     * the interrupt path works end to end. */
    kprintf("verifying timer (waiting for 300 ticks = 3s at 100 Hz)...\n");

    uint32_t last_report = 0;
    while (pit_ticks() < 300) {
        uint32_t t = pit_ticks();
        if (t - last_report >= 100) {
            last_report = t;
            kprintf("  tick %u  (%u s)\n", t, t / pit_hz());
        }
        __asm__ volatile ("hlt");   /* sleep until the next interrupt */
    }

    kprintf("\ntimer verified: %u ticks observed.\n", pit_ticks());
    kprintf("interrupt plumbing verified. halting.\n");

    for (;;)
        __asm__ volatile ("hlt");
}
