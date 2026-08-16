/* kmain.c — the kernel's C entry point.
 *
 * boot.asm hands control here with a stack established and GRUB's boot
 * information available. Subsystems are brought up in dependency order:
 * output first (so failures are visible), then descriptor tables, then
 * interrupt routing, then devices, then memory.
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

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Defined by linker.ld at the end of the kernel image. Free memory starts
 * here, so this is where the heap goes. */
extern uint32_t kernel_end;

#define HEAP_SIZE (1024 * 1024)

static void verify_heap(void)
{
    heap_stats_t before, after;

    kprintf("\nverifying heap:\n");

    heap_get_stats(&before);

    /* Non-overlap: two live allocations must occupy distinct memory. */
    uint8_t *a = kmalloc(64 * 1024);
    uint8_t *b = kmalloc(64 * 1024);

    if (!a || !b)
        panic("heap: initial allocations failed");

    kprintf("  alloc 64K -> %p\n", a);
    kprintf("  alloc 64K -> %p\n", b);

    uint32_t gap = (uint32_t)b - (uint32_t)a;
    kprintf("  [%s] blocks do not overlap (gap %u bytes)\n",
            gap >= 64 * 1024 ? "PASS" : "FAIL", gap);

    /* Integrity: writing to one block must not disturb the other. */
    for (int i = 0; i < 64 * 1024; i++) a[i] = 0xAA;
    for (int i = 0; i < 64 * 1024; i++) b[i] = 0xBB;

    int intact = 1;
    for (int i = 0; i < 64 * 1024; i++)
        if (a[i] != 0xAA) { intact = 0; break; }

    kprintf("  [%s] block A intact after writing block B\n",
            intact ? "PASS" : "FAIL");

    /* Reuse: freeing a block should make its address available again. */
    kfree(a);
    uint8_t *c = kmalloc(32 * 1024);
    kprintf("  [%s] freed block reused (%p)\n",
            (void *)c == (void *)a ? "PASS" : "FAIL", c);

    /* Coalescing: after freeing everything, the heap should be one big block
     * again rather than a scatter of fragments. */
    kfree(b);
    kfree(c);
    heap_get_stats(&after);

    kprintf("  [%s] all blocks coalesced (%u free block%s, largest %u KB)\n",
            after.free_blocks == 1 ? "PASS" : "FAIL",
            after.free_blocks, after.free_blocks == 1 ? "" : "s",
            after.largest_free / 1024);

    /* Bounds: allocating more than exists must fail cleanly, not wrap or
     * hand back memory the heap does not own. */
    void *huge = kmalloc(HEAP_SIZE * 2);
    kprintf("  [%s] oversized allocation returns NULL\n",
            huge == 0 ? "PASS" : "FAIL");

    kprintf("  [%s] heap structure intact\n", heap_check() == 0 ? "PASS" : "FAIL");

    (void)before;
}

void kmain(uint32_t magic, uint32_t *mb_info)
{
    /* Output before anything else. Every failure after this point is only
     * diagnosable because these two channels already work. */
    console_init();

    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("MyOS v0.3 - bare metal x86 (i386, protected mode)\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

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

    gdt_init();
    isr_init();
    idt_init();
    pic_init();
    pit_init(100);
    kbd_init();

    /* Page-align the heap start so it never shares a page with kernel data. */
    uint32_t heap_start = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFU;
    heap_init(heap_start, HEAP_SIZE);

    sti();
    kprintf("interrupts       : enabled\n");

    verify_heap();

    kprintf("\nsubsystems verified. type to test the keyboard (esc halts):\n> ");

    for (;;) {
        char c = kbd_getchar();

        if (c == 27) {              /* escape */
            kprintf("\n\nhalting.\n");
            break;
        }

        if (c == '\n')
            kprintf("\n> ");
        else
            kputc(c);
    }

    for (;;)
        __asm__ volatile ("hlt");
}
