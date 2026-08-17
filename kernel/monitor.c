#include "monitor.h"
#include "task.h"
#include "heap.h"
#include "pit.h"
#include "keyboard.h"
#include "console.h"
#include "vga.h"

#include <stdint.h>

#define BAR_WIDTH 20

/* Draw a proportional bar, e.g. [########------------]. */
static void draw_bar(uint32_t used, uint32_t total, int width)
{
    uint32_t filled = total ? (used * (uint32_t)width) / total : 0;

    kputc('[');
    for (int i = 0; i < width; i++)
        kputc(i < (int)filled ? '#' : '-');
    kputc(']');
}

/* Box width including both border characters. Chosen to sit comfortably
 * inside an 80-column screen with room to spare. */
#define BOX_W 64

/* Draw a full-width horizontal rule. */
static void rule(void)
{
    kputc('+');
    for (int i = 0; i < BOX_W - 2; i++)
        kputc('-');
    kputc('+');
    kputc('\n');
}

/* Close a row, padding out to the exact box width first.
 *
 * The padding is measured from the cursor rather than counted by hand.
 * Hand-counting is what produced the original misalignment: three different
 * format strings that were each supposed to be the same width and were not,
 * so the right-hand border landed in three different columns. */
static void row_end(void)
{
    while (vga_get_x() < BOX_W - 1)
        kputc(' ');

    kputc('|');
    kputc('\n');
}

void monitor_draw(void)
{
    uint32_t ticks = pit_ticks();
    uint32_t hz    = pit_hz();
    uint32_t secs  = hz ? ticks / hz : 0;

    heap_stats_t heap;
    heap_get_stats(&heap);

    rule();

    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("| MyOS");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    kprintf("   bare metal x86            uptime %02u:%02u:%02u",
            secs / 3600, (secs / 60) % 60, secs % 60);
    row_end();

    rule();

    kprintf("| %-4s %-12s %-9s %9s %8s", "PID", "NAME", "STATE", "TICKS", "STACK");
    row_end();

    for (task_t *t = task_list(); t; t = t->next) {
        kprintf("| %-4d %-12s ", t->id, t->name);

        /* Colour the running task so it stands out as the display refreshes. */
        if (t->state == TASK_RUNNING)
            vga_set_color(VGA_LGREEN, VGA_BLACK);
        else if (t->state == TASK_BLOCKED)
            vga_set_color(VGA_YELLOW, VGA_BLACK);

        kprintf("%-9s", task_state_name(t->state));
        vga_set_color(VGA_LGREY, VGA_BLACK);

        kprintf(" %9u %8u", t->ticks, t->stack_size);
        row_end();
    }

    rule();

    kprintf("| heap  ");
    draw_bar(heap.used_bytes, heap.total_bytes, BAR_WIDTH);
    kprintf(" %4u / %4u KB", heap.used_bytes / 1024, heap.total_bytes / 1024);
    row_end();

    kprintf("| irqs  timer %-7u kbd %-6u switches %-7u",
            ticks, kbd_irq_count(), task_switch_count());
    row_end();

    kprintf("| tasks %-3d  preemption %-3s  free blocks %-3u",
            task_count(),
            task_preempt_enabled() ? "on" : "OFF",
            heap.free_blocks);
    row_end();

    rule();
}

void monitor_run(void)
{
    kprintf("(press any key to exit)\n\n");

    for (;;) {
        if (kbd_available()) {
            (void)kbd_poll();
            return;
        }

        /* Home the cursor rather than clearing, so the display updates in
         * place instead of flickering through a blank screen. */
        console_home();
        monitor_draw();

        /* Redraw roughly 4 times a second. Yielding rather than spinning keeps
         * background tasks running while the monitor is open, which is the
         * point of watching it. */
        uint32_t until = pit_ticks() + 25;
        while (pit_ticks() < until) {
            if (kbd_available()) {
                (void)kbd_poll();
                return;
            }
            task_yield();
        }
    }
}
