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

void monitor_draw(void)
{
    uint32_t ticks = pit_ticks();
    uint32_t hz    = pit_hz();
    uint32_t secs  = hz ? ticks / hz : 0;

    heap_stats_t heap;
    heap_get_stats(&heap);

    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("+---- MyOS ------------------------------ uptime %02u:%02u:%02u ----+\n",
            secs / 3600, (secs / 60) % 60, secs % 60);
    vga_set_color(VGA_LGREY, VGA_BLACK);

    kprintf("| %-4s %-12s %-8s %8s  %-10s |\n",
            "PID", "NAME", "STATE", "TICKS", "STACK");

    for (task_t *t = task_list(); t; t = t->next) {
        kprintf("| %-4d %-12s ", t->id, t->name);

        /* Colour the running task so it stands out as the display refreshes. */
        if (t->state == TASK_RUNNING)
            vga_set_color(VGA_LGREEN, VGA_BLACK);
        else if (t->state == TASK_BLOCKED)
            vga_set_color(VGA_YELLOW, VGA_BLACK);

        kprintf("%-8s", task_state_name(t->state));
        vga_set_color(VGA_LGREY, VGA_BLACK);

        kprintf(" %8u  %-10u |\n", t->ticks, t->stack_size);
    }

    kprintf("+---------------------------------------------------------+\n");

    kprintf("| heap  ");
    draw_bar(heap.used_bytes, heap.total_bytes, BAR_WIDTH);
    kprintf("  %4u / %4u KB          |\n",
            heap.used_bytes / 1024, heap.total_bytes / 1024);

    kprintf("| irqs  timer %-8u  kbd %-6u  switches %-8u |\n",
            ticks, kbd_irq_count(), task_switch_count());

    kprintf("| tasks %-3d  preemption %-3s  free blocks %-3u          |\n",
            task_count(),
            task_preempt_enabled() ? "on" : "OFF",
            heap.free_blocks);

    kprintf("+---------------------------------------------------------+\n");
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
