/* kmain.c — the kernel's C entry point.
 *
 * boot.asm hands control here with a stack established and GRUB's boot
 * information available. Subsystems are brought up in dependency order:
 * output first (so failures are visible), then descriptor tables, then
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
#include "task.h"
#include "string.h"

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Defined by linker.ld at the end of the kernel image. */
extern uint32_t kernel_end;

#define HEAP_SIZE (1024 * 1024)

/* ------------------------------------------------------------------------ */
/* Scheduler verification                                                    */
/* ------------------------------------------------------------------------ */

static volatile int      printers_done;
static volatile uint32_t quiet_counter;
static volatile int      quiet_stop;
static volatile int      quiet_done;

/* Each printing task spins in a tight loop and never yields. Any interleaving
 * in the output is therefore caused by the timer forcing a switch — it cannot
 * be produced by cooperation, because there is none. */
static void printer_body(char label, int bursts)
{
    for (int b = 0; b < bursts; b++) {
        for (volatile uint32_t i = 0; i < 4000000; i++)
            ;
        kputc(label);
    }

    printers_done++;
}

static void printer_a(void) { printer_body('A', 10); }
static void printer_b(void) { printer_body('B', 10); }
static void printer_c(void) { printer_body('C', 10); }

/* Produces no output whatsoever. If its counter has advanced by the end, the
 * only possible explanation is that the scheduler gave it real CPU time —
 * there is nothing here that could be faked by a print statement. */
static void quiet_task(void)
{
    while (!quiet_stop)
        quiet_counter++;

    quiet_done = 1;
}

static void verify_scheduler(void)
{
    kprintf("\nverifying scheduler:\n");
    kprintf("  spawning 3 printing tasks (tight loops, no yields) + 1 silent task\n");
    kprintf("  output: ");

    task_create("printer_a", printer_a);
    task_create("printer_b", printer_b);
    task_create("printer_c", printer_c);
    task_create("quiet",     quiet_task);

    while (printers_done < 3)
        __asm__ volatile ("hlt");

    quiet_stop = 1;
    while (!quiet_done)
        __asm__ volatile ("hlt");

    kprintf("\n\n");

    uint32_t switches = task_switch_count();
    kprintf("  [%s] context switches occurred (%u)\n",
            switches > 0 ? "PASS" : "FAIL", switches);

    kprintf("  [%s] silent task received CPU time (counter = %u)\n",
            quiet_counter > 0 ? "PASS" : "FAIL", quiet_counter);

    /* Fairness: every task should have accumulated a similar share of ticks.
     * Round robin makes no promises beyond "everyone runs", so this checks
     * that no task was starved rather than exact equality. */
    uint32_t min_ticks = 0xFFFFFFFF, max_ticks = 0;
    int counted = 0;

    kprintf("\n  per-task CPU time:\n");
    for (task_t *t = task_list(); t; t = t->next) {
        kprintf("    %-10s id=%d  state=%-8s ticks=%u\n",
                t->name, t->id, task_state_name(t->state), t->ticks);

        if (t->id != 1) {   /* exclude the kernel task, which mostly sleeps */
            if (t->ticks < min_ticks) min_ticks = t->ticks;
            if (t->ticks > max_ticks) max_ticks = t->ticks;
            counted++;
        }
    }

    if (counted > 0 && min_ticks > 0) {
        /* Allow a wide margin: tasks finish at different times, so the last
         * one alive necessarily accrues extra ticks. */
        bool fair = max_ticks <= min_ticks * 4;
        kprintf("\n  [%s] no task starved (min %u, max %u ticks)\n",
                fair ? "PASS" : "FAIL", min_ticks, max_ticks);
    } else {
        kprintf("\n  [FAIL] some task received no CPU time at all\n");
    }

    task_t *culprit = 0;
    kprintf("  [%s] all stack guards intact\n",
            task_check_stacks(&culprit) ? "PASS" : "FAIL");

    task_reap();
    kprintf("  [%s] finished tasks reaped (%d remaining)\n",
            task_count() == 1 ? "PASS" : "FAIL", task_count());

    kprintf("  [%s] heap intact after task churn\n",
            heap_check() == 0 ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------------ */

static void verify_heap(void)
{
    kprintf("\nverifying heap:\n");

    uint8_t *a = kmalloc(64 * 1024);
    uint8_t *b = kmalloc(64 * 1024);

    if (!a || !b)
        panic("heap: initial allocations failed");

    uint32_t gap = (uint32_t)b - (uint32_t)a;
    kprintf("  [%s] blocks do not overlap (%p, %p)\n",
            gap >= 64 * 1024 ? "PASS" : "FAIL", a, b);

    memset(a, 0xAA, 64 * 1024);
    memset(b, 0xBB, 64 * 1024);

    int intact = 1;
    for (int i = 0; i < 64 * 1024; i++)
        if (a[i] != 0xAA) { intact = 0; break; }

    kprintf("  [%s] writing block B left block A untouched\n",
            intact ? "PASS" : "FAIL");

    kfree(a);
    uint8_t *c = kmalloc(32 * 1024);
    kprintf("  [%s] freed address reused (%p)\n",
            (void *)c == (void *)a ? "PASS" : "FAIL", c);

    kfree(b);
    kfree(c);

    heap_stats_t st;
    heap_get_stats(&st);
    kprintf("  [%s] all blocks coalesced (%u free block%s, largest %u KB)\n",
            st.free_blocks == 1 ? "PASS" : "FAIL",
            st.free_blocks, st.free_blocks == 1 ? "" : "s",
            st.largest_free / 1024);

    kprintf("  [%s] oversized allocation returns NULL\n",
            kmalloc(HEAP_SIZE * 2) == 0 ? "PASS" : "FAIL");

    kprintf("  [%s] heap structure intact\n", heap_check() == 0 ? "PASS" : "FAIL");
}

void kmain(uint32_t magic, uint32_t *mb_info)
{
    console_init();

    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("MyOS v0.4 - bare metal x86 (i386, protected mode)\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic: expected %08x, got %08x",
              MULTIBOOT_BOOTLOADER_MAGIC, magic);

    kprintf("multiboot        : %08x OK\n", magic);

    if (mb_info && (mb_info[0] & 0x1))
        kprintf("memory           : %u KB low, %u KB high (%u MB total)\n",
                mb_info[1], mb_info[2], (mb_info[1] + mb_info[2]) / 1024);

    gdt_init();
    isr_init();
    idt_init();
    pic_init();
    pit_init(100);
    kbd_init();

    uint32_t heap_start = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFU;
    heap_init(heap_start, HEAP_SIZE);

    task_init();
    pit_on_tick(task_tick);     /* the timer now drives the scheduler */

    sti();
    kprintf("interrupts       : enabled\n");

    verify_heap();
    verify_scheduler();

    kprintf("\nall subsystems verified. halting.\n");

    for (;;)
        __asm__ volatile ("hlt");
}
