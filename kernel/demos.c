#include "demos.h"
#include "task.h"
#include "sync.h"
#include "console.h"
#include "pit.h"
#include "paging.h"

/* ---- race demonstration ------------------------------------------------- */

static volatile uint32_t shared_counter;
static volatile int      racers_done;
static bool              race_locked;
static mutex_t           race_mutex;

/* How many spin iterations fit inside one timer tick on this machine.
 *
 * Measured rather than hardcoded. A fixed count means the width of the race
 * window depends on how fast the host CPU happens to be: too small and the
 * critical section always completes untouched, too large and every single
 * iteration loses an update. Both extremes are deterministic, and a result
 * that is identical on every run is exactly what a fabricated demo would
 * also produce. Calibrating against the same clock that drives preemption
 * keeps the window meaningful on any machine. */
static uint32_t spins_per_tick;

static void calibrate_spin(void)
{
    if (spins_per_tick)
        return;

    uint32_t t = pit_ticks();
    while (pit_ticks() == t)        /* align to a tick boundary first */
        ;

    t = pit_ticks();

    uint32_t count = 0;
    while (pit_ticks() == t)
        count++;

    spins_per_tick = count ? count : 1000;
}

/* Small linear congruential generator. Only needs to be irregular, not good. */
static uint32_t rng_state = 0x2545F491;

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static void racer(void)
{
    for (uint32_t i = 0; i < RACE_ITERATIONS; i++) {
        if (race_locked)
            mutex_lock(&race_mutex);

        /* A deliberately non-atomic read-modify-write.
         *
         * The gap between reading and writing is what makes the race possible,
         * and it has to be wide enough that a context switch actually lands in
         * it. A fixed spin count is the obvious approach and the wrong one:
         * how long it takes depends entirely on how fast the host CPU is, and
         * on a fast machine the whole sequence finishes inside one timeslice.
         * The counter then comes out correct and the demo silently proves
         * nothing.
         *
         * Waiting for the tick counter to change instead ties the width of the
         * window to the same clock that drives preemption, so a switch is
         * guaranteed to occur here regardless of machine speed. */
        uint32_t value = shared_counter;

        /* Hold the update half-finished for a randomised span of roughly zero
         * to two timer ticks. Sometimes the write lands before the next
         * preemption and the increment survives; sometimes it does not and the
         * update is lost. That intermediate probability is what makes the
         * final total differ from run to run. */
        uint32_t spin = rng_next() % (2 * spins_per_tick + 1);

        for (volatile uint32_t d = 0; d < spin; d++)
            ;   /* not task_yield(): preemption must be what interrupts us */

        shared_counter = value + 1;

        if (race_locked)
            mutex_unlock(&race_mutex);
    }

    racers_done++;
}

uint32_t race_run(bool use_lock)
{
    calibrate_spin();

    shared_counter = 0;
    racers_done    = 0;
    race_locked    = use_lock;

    mutex_init(&race_mutex);

    task_create("racer_1", racer);
    task_create("racer_2", racer);

    while (racers_done < 2)
        task_yield();

    task_reap();

    return shared_counter;
}

/* ---- printing tasks ----------------------------------------------------- */

static volatile int printers_done;
static volatile int printers_expected;

static void printer_body(char label)
{
    for (int burst = 0; burst < 10; burst++) {
        for (volatile uint32_t i = 0; i < 3000000; i++)
            ;
        kputc(label);
    }

    printers_done++;
}

static void printer_a(void) { printer_body('A'); }
static void printer_b(void) { printer_body('B'); }
static void printer_c(void) { printer_body('C'); }
static void printer_d(void) { printer_body('D'); }

void spawn_printers(int n)
{
    static task_entry_t entries[4] = { printer_a, printer_b, printer_c, printer_d };
    static const char  *names[4]   = { "print_a", "print_b", "print_c", "print_d" };

    if (n < 1) n = 1;
    if (n > 4) n = 4;

    printers_done     = 0;
    printers_expected = n;

    for (int i = 0; i < n; i++)
        task_create(names[i], entries[i]);

    while (printers_done < printers_expected)
        task_yield();

    task_reap();
}

/* ---- silent task -------------------------------------------------------- */

static volatile uint32_t quiet_counter;
static volatile int      quiet_should_stop;
static volatile int      quiet_finished;

static void quiet_body(void)
{
    while (!quiet_should_stop)
        quiet_counter++;

    quiet_finished = 1;
}

void quiet_start(void)
{
    quiet_counter     = 0;
    quiet_should_stop = 0;
    quiet_finished    = 0;

    task_create("quiet", quiet_body);
}

uint32_t quiet_stop_and_read(void)
{
    quiet_should_stop = 1;

    while (!quiet_finished)
        task_yield();

    task_reap();

    return quiet_counter;
}

/* ---- deliberate faults -------------------------------------------------- */

static void fault_div0(void)
{
    kprintf("  [task] dividing by zero...\n");

    /* This must be written in assembly, not C.
     *
     * The obvious C version -- `volatile int z = 0; int r = 1 / z;` -- does
     * not fault, because gcc never emits a division at all. Dividing by zero
     * is undefined behaviour, so the compiler may assume it cannot happen,
     * and it rewrites `1 / x` into a branchless select: the result is x for
     * x = 1 or -1, and zero otherwise. Marking the operand volatile forces
     * the load but not the division, so it does not help either.
     *
     * Writing the div instruction directly leaves the compiler no room to
     * reason about it, and the CPU raises exception 0 as intended. */
    __asm__ volatile (
        "xorl %%edx, %%edx  \n"     /* high half of the dividend */
        "movl $1, %%eax     \n"     /* low half  */
        "xorl %%ecx, %%ecx  \n"     /* divisor = 0 */
        "divl %%ecx         \n"     /* -> #DE, exception vector 0 */
        :
        :
        : "eax", "ecx", "edx", "cc"
    );

    kprintf("  [task] still alive - the fault did not fire!\n");
}

static void fault_opcode(void)
{
    kprintf("  [task] executing an undefined instruction (ud2)...\n");

    __asm__ volatile ("ud2");

    kprintf("  [task] still alive - the fault did not fire!\n");
}

static void fault_gpf(void)
{
    kprintf("  [task] loading a segment selector past the end of the GDT...\n");

    /* Selector 0x80 indexes well beyond our six-entry GDT, so the CPU raises
     * a general protection fault with the offending selector as error code. */
    __asm__ volatile ("mov $0x80, %ax; mov %ax, %ds");

    kprintf("  [task] still alive - the fault did not fire!\n");
}

/* Address used by the "page" variant, set just before the task is created. */
static volatile uint32_t fault_target;

static void touch_address(uint32_t addr)
{
    /* Inline assembly for the same reason the division is: dereferencing an
     * address the compiler believes is invalid is undefined behaviour, and it
     * is free to delete the access rather than emit it. */
    __asm__ volatile ("movl (%0), %%eax"
                      :
                      : "r"(addr)
                      : "eax", "memory");
}

static void fault_null(void)
{
    kprintf("  [task] dereferencing a null pointer...\n");
    touch_address(0);
    kprintf("  [task] still alive - the fault did not fire!\n");
}

static void fault_page(void)
{
    kprintf("  [task] reading unmapped address %08x...\n", fault_target);
    touch_address(fault_target);
    kprintf("  [task] still alive - the fault did not fire!\n");
}

void fault_spawn(const char *kind, uint32_t addr)
{
    task_entry_t entry;

    if (kind[0] == 'd')
        entry = fault_div0;
    else if (kind[0] == 'o')
        entry = fault_opcode;
    else if (kind[0] == 'g')
        entry = fault_gpf;
    else if (kind[0] == 'n')
        entry = fault_null;
    else if (kind[0] == 'p') {
        if (!paging_enabled()) {
            kprintf("fault: paging is not enabled, every address is valid\n");
            return;
        }
        fault_target = addr ? addr : 0xE0000000u;
        entry = fault_page;
    } else {
        kprintf("fault: unknown kind '%s'\n", kind);
        kprintf("       try div0, opcode, gpf, null or page <addr>\n");
        return;
    }

    int before = task_count();

    kprintf("\nspawning a task that will fault:\n");

    task_t *t = task_create("faulter", entry);
    if (!t) {
        kprintf("fault: could not create task\n");
        return;
    }

    /* Wait for it to die. It never sets a completion flag — the fault handler
     * terminates it — so watch the task list instead. */
    uint32_t give_up_at = pit_ticks() + 200;
    while (pit_ticks() < give_up_at) {
        task_yield();

        if (t->state == TASK_DEAD)
            break;
    }

    task_reap();

    kprintf("\nshell is still running: %d task%s before, %d after.\n\n",
            before, before == 1 ? "" : "s", task_count());
}
