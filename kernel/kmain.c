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
#include "mouse.h"
#include "heap.h"
#include "paging.h"
#include "task.h"
#include "syscall.h"
#include "shell.h"
#include "selftest.h"
#include "multiboot.h"
#include "fb.h"
#include "fbcon.h"

#include <stdint.h>

/* Placed by linker.ld at the end of the kernel image; free memory starts here. */
extern uint32_t kernel_end;

/* 32 MB. The framebuffer backbuffer alone is 8.3 MB at 1920x1080x32, which
 * the original 1 MB heap could not have held. */
#define HEAP_SIZE (32 * 1024 * 1024)

static void banner(void)
{
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("\n");
    kprintf("  MyOS v1.0 - a bare metal x86 kernel\n");
    vga_set_color(VGA_DGREY, VGA_BLACK);
    kprintf("  i386 protected mode, no operating system underneath\n\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
}

void kmain(uint32_t magic, multiboot_info_t *mb)
{
    console_init();
    banner();

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic: expected %08x, got %08x",
              MULTIBOOT_BOOTLOADER_MAGIC, magic);

    kprintf("multiboot        : %08x OK\n", magic);

    if (mb->flags & MB_INFO_MEMORY)
        kprintf("memory           : %u KB low, %u KB high (%u MB total)\n",
                mb->mem_lower, mb->mem_upper,
                (mb->mem_lower + mb->mem_upper) / 1024);

    gdt_init();
    isr_init();
    idt_init();     /* from here a fault is reported instead of resetting */
    pic_init();
    pit_init(100);
    kbd_init();
    mouse_init();   /* same 8042 controller as the keyboard, on IRQ 12 */

    /* Paging goes after the IDT so that a page fault has somewhere to land,
     * and before the heap so that heap memory is mapped from the start.
     * The framebuffer is mapped at the same time: it sits far above RAM, and
     * writing to it unmapped would fault on the first pixel. */
    uint32_t ram_bytes = 16 * 1024 * 1024;
    if (mb->flags & MB_INFO_MEMORY)
        ram_bytes = (mb->mem_lower + mb->mem_upper) * 1024;

    uint32_t fb_addr = 0, fb_size = 0;
    if (mb->flags & MB_INFO_FRAMEBUFFER) {
        fb_addr = (uint32_t)mb->framebuffer_addr;
        fb_size = mb->framebuffer_pitch * mb->framebuffer_height;
    }

    paging_init(ram_bytes, fb_addr, fb_size);

    /* Page-align the heap so it never shares a page with kernel data. */
    uint32_t heap_start = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFU;
    heap_init(heap_start, HEAP_SIZE);

    /* Now that memory is mapped and allocatable, bring up the framebuffer and
     * move the console onto it. Everything printed before this point went to
     * the serial port only, because in a graphics mode the VGA text buffer
     * displays nothing. The serial log therefore holds the complete boot
     * record even though the screen does not. */
    if (fb_init(mb)) {
        fbcon_init();
        banner();       /* repaint the header now that there is a screen */
        kprintf("display          : %ux%u framebuffer console, %dx%d characters\n",
                fb_width(), fb_height(), fbcon_cols(), fbcon_rows());
    }

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
