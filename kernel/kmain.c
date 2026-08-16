/* kmain.c — the kernel's C entry point.
 *
 * boot.asm hands control here with a stack established and GRUB's boot
 * information available. Subsystems come up in dependency order: output first
 * so that later failures are visible at all, then descriptor tables, then
 * interrupt routing, then devices, then memory, then tasks.
 */
#include "console.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "heap.h"
#include "paging.h"
#include "task.h"
#include "syscall.h"
#include "shell.h"
#include "selftest.h"

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Placed by linker.ld at the end of the kernel image; free memory starts here. */
extern uint32_t kernel_end;

#define HEAP_SIZE (1024 * 1024)

static void banner(void)
{
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("\n");
    kprintf("  MyOS v1.0 - a bare metal x86 kernel\n");
    vga_set_color(VGA_DGREY, VGA_BLACK);
    kprintf("  i386 protected mode, no operating system underneath\n\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
}

void kmain(uint32_t magic, uint32_t *mb_info)
{
    console_init();
    banner();

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic: expected %08x, got %08x",
              MULTIBOOT_BOOTLOADER_MAGIC, magic);

    kprintf("multiboot        : %08x OK\n", magic);

    if (mb_info && (mb_info[0] & 0x1))
        kprintf("memory           : %u KB low, %u KB high (%u MB total)\n",
                mb_info[1], mb_info[2], (mb_info[1] + mb_info[2]) / 1024);

    gdt_init();
    isr_init();
    idt_init();     /* from here a fault is reported instead of resetting */
    pic_init();
    pit_init(100);
    kbd_init();

    /* Paging goes after the IDT so that a page fault has somewhere to land,
     * and before the heap so that heap memory is mapped from the start. */
    uint32_t ram_bytes = 16 * 1024 * 1024;
    if (mb_info && (mb_info[0] & 0x1))
        ram_bytes = (mb_info[1] + mb_info[2]) * 1024;

    paging_init(ram_bytes);

    /* Page-align the heap so it never shares a page with kernel data. */
    uint32_t heap_start = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFU;
    heap_init(heap_start, HEAP_SIZE);

    task_init();
    syscall_init();
    pit_on_tick(task_tick);     /* the timer now drives the scheduler */

    sti();
    kprintf("interrupts       : enabled\n");

    /* Run the full verification suite at boot. It doubles as the automated
     * check behind `make test`, which greps this output for failures. */
    int failures = selftest_run();

    if (failures > 0) {
        vga_set_color(VGA_LRED, VGA_BLACK);
        kprintf("\nWARNING: %d self-test%s failed. the shell will still start.\n",
                failures, failures == 1 ? "" : "s");
        vga_set_color(VGA_LGREY, VGA_BLACK);
    }

    shell_run();    /* never returns */
}
