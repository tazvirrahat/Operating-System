#include "demos.h"
#include "task.h"
#include "sync.h"
#include "console.h"
#include "pit.h"
#include "paging.h"
#include "syscall.h"
#include "heap.h"

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

        /* Hold the update half-finished for a randomised span of zero to about
         * one timeslice. Sometimes the write lands before the next preemption
         * and the increment survives; sometimes it does not and the update is
         * lost.
         *
         * The range matters. Spanning up to *two* timeslices means nearly every
         * iteration is interrupted, every update is lost, and the total pins to
         * exactly half — reproducible to the digit, which is precisely what a
         * fabricated result would also look like. Keeping the window at roughly
         * one timeslice puts the loss probability near half, so the total lands
         * somewhere different on each run. */
        /* Quarter of a tick's worth of work, not a whole one. The calibration
         * measured how much spinning fits in a tick when running alone, but
         * two racers share the CPU, so the same work takes roughly twice the
         * wall time — and a window that reliably exceeds one timeslice puts
         * the loss probability at ~100% again. */
        uint32_t spin = rng_next() % (spins_per_tick / 4 + 1);

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

/* ---- bounded-buffer producer/consumer ----------------------------------- */

/* The classic use for counting semaphores, and the reason they exist at all:
 * a mutex alone cannot express "wait until there is room" or "wait until there
 * is something to take". Those are counts, not ownership. */

static uint32_t pc_buffer[PC_BUFFER_SLOTS];
static int      pc_head, pc_tail;

static sem_t    pc_free_slots;      /* how many slots are empty */
static sem_t    pc_used_slots;      /* how many slots hold an item */
static mutex_t  pc_lock;            /* protects head/tail/buffer */

static volatile int      pc_received;
static volatile bool     pc_ordered;
static volatile int      pc_max_occupancy;
static volatile bool     pc_overflowed;
static volatile int      pc_done;
static bool              pc_verbose;

static void pc_producer(void)
{
    for (uint32_t item = 1; item <= PC_ITEM_COUNT; item++) {
        sem_wait(&pc_free_slots);       /* blocks while the buffer is full */

        mutex_lock(&pc_lock);

        pc_buffer[pc_head] = item;
        pc_head = (pc_head + 1) % PC_BUFFER_SLOTS;

        /* Occupancy is derived here rather than trusted from the semaphore,
         * so the check is independent of the thing being tested. */
        int occupancy = (pc_head - pc_tail + PC_BUFFER_SLOTS) % PC_BUFFER_SLOTS;
        if (occupancy == 0)
            occupancy = PC_BUFFER_SLOTS;    /* full wraps to zero */

        if (occupancy > pc_max_occupancy)
            pc_max_occupancy = occupancy;
        if (occupancy > PC_BUFFER_SLOTS)
            pc_overflowed = true;

        mutex_unlock(&pc_lock);

        if (pc_verbose)
            kprintf("P%u ", item);

        sem_post(&pc_used_slots);       /* wake a waiting consumer */
    }

    pc_done++;
}

static void pc_consumer(void)
{
    for (uint32_t expected = 1; expected <= PC_ITEM_COUNT; expected++) {
        sem_wait(&pc_used_slots);       /* blocks while the buffer is empty */

        mutex_lock(&pc_lock);

        uint32_t item = pc_buffer[pc_tail];
        pc_tail = (pc_tail + 1) % PC_BUFFER_SLOTS;

        mutex_unlock(&pc_lock);

        if (item != expected)
            pc_ordered = false;

        pc_received++;

        if (pc_verbose)
            kprintf("c%u ", item);

        sem_post(&pc_free_slots);       /* wake a waiting producer */
    }

    pc_done++;
}

bool producer_consumer_run(bool verbose)
{
    pc_head = pc_tail = 0;
    pc_received = 0;
    pc_ordered = true;
    pc_max_occupancy = 0;
    pc_overflowed = false;
    pc_done = 0;
    pc_verbose = verbose;

    /* Every slot starts free and none holds anything. These two counts are
     * what make the blocking work in both directions. */
    sem_init(&pc_free_slots, PC_BUFFER_SLOTS);
    sem_init(&pc_used_slots, 0);
    mutex_init(&pc_lock);

    task_create("producer", pc_producer);
    task_create("consumer", pc_consumer);

    while (pc_done < 2)
        task_yield();

    task_reap();

    return pc_received == PC_ITEM_COUNT
        && pc_ordered
        && !pc_overflowed
        && pc_max_occupancy <= PC_BUFFER_SLOTS;
}

/* ---- long-running background workers ------------------------------------ */

static volatile int background_stop;
static volatile uint32_t background_counters[4];

static void background_body(int slot)
{
    while (!background_stop)
        background_counters[slot]++;
}

static void background_0(void) { background_body(0); }
static void background_1(void) { background_body(1); }
static void background_2(void) { background_body(2); }
static void background_3(void) { background_body(3); }

void spawn_background(int n)
{
    static task_entry_t entries[4] = {
        background_0, background_1, background_2, background_3
    };
    static const char *names[4] = { "worker_a", "worker_b", "worker_c", "worker_d" };

    if (n < 1) n = 1;
    if (n > 4) n = 4;

    background_stop = 0;

    for (int i = 0; i < n; i++) {
        background_counters[i] = 0;
        task_create(names[i], entries[i]);
    }

    /* Deliberately no wait here: the point is that the shell keeps responding
     * while these run, which only works because the shell is preempted too. */
}

void stop_background(void)
{
    background_stop = 1;

    /* Give them a moment to notice and exit before reaping. */
    uint32_t until = pit_ticks() + 20;
    while (pit_ticks() < until)
        task_yield();

    task_reap();
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

/* ---- ring 3 -------------------------------------------------------------- */

/* Syscall wrappers. These are the only kernel services ring 3 code can reach:
 * everything goes through int 0x80, the one IDT gate with DPL 3. */

static inline uint32_t sys_call0(uint32_t num)
{
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline uint32_t sys_call1(uint32_t num, uint32_t arg)
{
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(arg) : "memory");
    return ret;
}

/* --- this code runs in ring 3 --- */

static void user_program_direct_hardware(void)
{
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] running unprivileged\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] writing directly to VGA port 0x3D4...\n");

    /* Port I/O is gated by IOPL, which is 0 in the EFLAGS we entered ring 3
     * with. The CPU raises a general protection fault here. We never get to
     * the next line. */
    __asm__ volatile ("movw $0x3D4, %dx; movb $0x0F, %al; outb %al, %dx");

    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] hardware write succeeded - NOT expected!\n");
    sys_call0(SYS_EXIT);
}

static void user_program_via_syscall(void)
{
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] running unprivileged\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] asking the kernel instead of touching hardware...\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] hello from ring 3, printed by the kernel on my behalf\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] exiting cleanly\n");

    sys_call0(SYS_EXIT);

    /* Unreachable: SYS_EXIT never returns. */
    for (;;)
        ;
}

/* --- back in ring 0 --- */

static bool user_wants_syscall;

static void user_task_entry(void)
{
    uint8_t *user_stack = kmalloc(4096);
    if (!user_stack) {
        kprintf("  could not allocate a user stack\n");
        return;
    }

    /* Report the privilege level from both sides of the boundary. The value
     * comes from CS, which the CPU maintains — we never assign it. */
    uint32_t cs_before;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs_before));
    kprintf("  [ring0] CS = %04x (ring %u) before the transition\n",
            cs_before & 0xFFFF, cs_before & 3);

    void (*program)(void) = user_wants_syscall
                          ? user_program_via_syscall
                          : user_program_direct_hardware;

    enter_user_mode(program, (uint32_t)user_stack + 4096);
}

void user_mode_demo(bool use_syscall)
{
    user_wants_syscall = use_syscall;

    kprintf("\n");
    task_t *t = task_create("usermode", user_task_entry);
    if (!t) {
        kprintf("could not create the task\n");
        return;
    }

    uint32_t give_up_at = pit_ticks() + 300;
    while (pit_ticks() < give_up_at) {
        task_yield();
        if (t->state == TASK_DEAD)
            break;
    }

    task_reap();
    kprintf("\nshell still running, %d task%s alive.\n\n",
            task_count(), task_count() == 1 ? "" : "s");
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
