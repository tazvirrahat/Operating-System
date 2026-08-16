#include "selftest.h"
#include "demos.h"
#include "task.h"
#include "heap.h"
#include "sync.h"
#include "pit.h"
#include "console.h"
#include "vga.h"
#include "string.h"

#include <stdbool.h>

static int passed;
static int failed;

/* Prints the verdict tag and tallies it. The description that follows is
 * printed by the caller, which keeps kprintf's varargs out of here. */
static void verdict(bool ok)
{
    if (ok) {
        vga_set_color(VGA_LGREEN, VGA_BLACK);
        kprintf("  [PASS] ");
        passed++;
    } else {
        vga_set_color(VGA_LRED, VGA_BLACK);
        kprintf("  [FAIL] ");
        failed++;
    }

    vga_set_color(VGA_LGREY, VGA_BLACK);
}

#define CHECK(cond, ...) do { verdict(cond); kprintf(__VA_ARGS__); } while (0)

/* ---- heap --------------------------------------------------------------- */

static void test_heap(void)
{
    kprintf("HEAP\n");

    heap_stats_t before;
    heap_get_stats(&before);

    uint8_t *a = kmalloc(4096);
    uint8_t *b = kmalloc(4096);

    CHECK(a && b, "allocations succeed\n");
    CHECK(a && b && ((uint32_t)b - (uint32_t)a) >= 4096,
          "allocations do not overlap\n");

    if (a && b) {
        memset(a, 0xAA, 4096);
        memset(b, 0xBB, 4096);

        bool intact = true;
        for (int i = 0; i < 4096; i++)
            if (a[i] != 0xAA) { intact = false; break; }

        CHECK(intact, "writing one block leaves its neighbour untouched\n");
    }

    void *addr_a = a;
    kfree(a);
    uint8_t *c = kmalloc(2048);
    CHECK((void *)c == addr_a, "freed address is reused\n");

    kfree(b);
    kfree(c);

    heap_stats_t after;
    heap_get_stats(&after);
    CHECK(after.free_blocks == before.free_blocks,
          "adjacent free blocks coalesce (%u blocks)\n", after.free_blocks);

    CHECK(kmalloc(0xF0000000) == 0, "exhaustion returns NULL, not garbage\n");
    CHECK(heap_check() == 0, "block list structurally intact\n");
}

/* ---- scheduler ---------------------------------------------------------- */

static void test_scheduler(void)
{
    kprintf("SCHEDULER\n");

    uint32_t switches_before = task_switch_count();

    /* A task that prints nothing at all. If its counter moved, the only
     * possible cause is the scheduler having given it real CPU time. */
    quiet_start();

    uint32_t until = pit_ticks() + 30;
    while (pit_ticks() < until)
        task_yield();

    uint32_t counter = quiet_stop_and_read();

    CHECK(counter > 0, "silent task received CPU time (counter = %u)\n", counter);
    CHECK(task_switch_count() > switches_before,
          "context switches occurred (%u total)\n", task_switch_count());

    /* Ablation. With preemption masked, a task that never yields must never
     * be taken off the CPU, so the switch count must stop moving. */
    task_set_preempt(false);

    uint32_t switches_at_off = task_switch_count();
    uint32_t spin_until = pit_ticks() + 20;
    while (pit_ticks() < spin_until)
        ;                                   /* no yield: purely preemption's job */

    bool no_switches = (task_switch_count() == switches_at_off);
    task_set_preempt(true);

    CHECK(no_switches,
          "preemption off -> no involuntary switches (ablation)\n");

    /* And with it back on, switching must resume. */
    uint32_t switches_at_on = task_switch_count();
    quiet_start();
    spin_until = pit_ticks() + 20;
    while (pit_ticks() < spin_until)
        ;
    (void)quiet_stop_and_read();

    CHECK(task_switch_count() > switches_at_on,
          "preemption on  -> switching resumes\n");

    task_t *culprit = 0;
    CHECK(task_check_stacks(&culprit), "all stack guard words intact\n");
}

/* ---- synchronisation ---------------------------------------------------- */

static void test_sync(void)
{
    kprintf("SYNCHRONISATION\n");

    /* Unlocked: the total must come out wrong. Just as important, it must come
     * out *differently* wrong across runs — a fixed wrong number would suggest
     * a deterministic bug rather than a genuine race. */
    uint32_t r1 = race_run(false);
    uint32_t r2 = race_run(false);
    uint32_t r3 = race_run(false);

    kprintf("         unlocked runs: %u, %u, %u (expected %u)\n",
            r1, r2, r3, (uint32_t)RACE_EXPECTED);

    CHECK(r1 < RACE_EXPECTED || r2 < RACE_EXPECTED || r3 < RACE_EXPECTED,
          "unsynchronised counter loses updates\n");

    CHECK(!(r1 == r2 && r2 == r3),
          "results vary between runs (genuine nondeterminism)\n");

    uint32_t l1 = race_run(true);
    uint32_t l2 = race_run(true);

    kprintf("         locked runs  : %u, %u (expected %u)\n",
            l1, l2, (uint32_t)RACE_EXPECTED);

    CHECK(l1 == RACE_EXPECTED && l2 == RACE_EXPECTED,
          "mutex makes the counter exact\n");

    /* Semaphore basics. */
    sem_t s;
    sem_init(&s, 2);
    sem_wait(&s);
    sem_wait(&s);
    sem_post(&s);
    CHECK(s.count == 1, "semaphore counts down and back up correctly\n");
}

/* ---- timer -------------------------------------------------------------- */

static void test_timer(void)
{
    kprintf("TIMER\n");

    uint32_t t0 = pit_ticks();

    while (pit_ticks() - t0 < 10)
        __asm__ volatile ("hlt");

    uint32_t elapsed = pit_ticks() - t0;

    CHECK(elapsed >= 10, "timer advances independently of the CPU (%u ticks)\n",
          elapsed);
    CHECK(pit_hz() == 100, "configured frequency reported correctly (%u Hz)\n",
          pit_hz());
}

/* ---- entry point -------------------------------------------------------- */

int selftest_run(void)
{
    passed = 0;
    failed = 0;

    kprintf("\n");
    test_timer();
    test_heap();
    test_scheduler();
    test_sync();

    kprintf("\n");
    if (failed == 0)
        vga_set_color(VGA_LGREEN, VGA_BLACK);
    else
        vga_set_color(VGA_LRED, VGA_BLACK);

    kprintf("%d passed, %d failed\n", passed, failed);
    vga_set_color(VGA_LGREY, VGA_BLACK);

    return failed;
}
