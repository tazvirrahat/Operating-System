#include "demos.h"
#include "task.h"
#include "sync.h"
#include "console.h"
#include "pit.h"
#include "paging.h"
#include "syscall.h"
#include "heap.h"
#include "string.h"
#include "isr.h"
#include "fs.h"

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
static volatile uint32_t background_checksums[4];
static volatile int background_alive;

/* How much arithmetic a worker does per wake-up, and how long it sleeps
 * afterwards. Together these set the duty cycle: the workers are meant to
 * be visible in Task Manager without owning the machine. */
#define WORKER_BATCH  8000000
#define WORKER_SLEEP  20

static void background_body(int slot)
{
    /* What these are for.
     *
     * They are load generators. Their job is to prove the scheduler is real
     * -- three threads that appear instantly, accrue CPU time at their own
     * separate rates, and let the desktop stay responsive throughout. They
     * are not a system service, and pretending they were would be worse than
     * saying so.
     *
     * They do perform genuine arithmetic rather than spinning on an empty
     * loop, so the ticks they are charged are ticks they actually earned:
     * each wake-up folds WORKER_BATCH values into a checksum.
     *
     * Then they sleep, and that part matters. The first version yielded and
     * then halted, which looks like it gives the CPU back and does not: a
     * halted task is still the *current* task, so the timer interrupt kept
     * charging it for time it spent doing nothing. Three workers therefore
     * appeared to eat the whole machine while the idle task starved, which
     * was an accounting artefact rather than real load. task_sleep marks the
     * task BLOCKED and reschedules, so the idle task genuinely runs and the
     * numbers in Task Manager mean what they say. */
    uint32_t sum = 0;

    while (!background_stop) {
        for (uint32_t i = 0; i < WORKER_BATCH; i++)
            sum = sum * 31u + i;

        background_checksums[slot] = sum;
        background_counters[slot]++;

        task_sleep(WORKER_SLEEP);
    }

    background_alive--;
}

static void background_0(void) { background_body(0); }
static void background_1(void) { background_body(1); }
static void background_2(void) { background_body(2); }
static void background_3(void) { background_body(3); }

int background_count(void)
{
    int n = 0;

    for (task_t *t = task_list(); t; t = t->next)
        if (t->state != TASK_DEAD && strncmp(t->name, "worker_", 7) == 0)
            n++;

    return n;
}

uint32_t background_counter(int slot)
{
    if (slot < 0 || slot > 3)
        return 0;
    return background_counters[slot];
}

int spawn_background(int n)
{
    static task_entry_t entries[4] = {
        background_0, background_1, background_2, background_3
    };
    static const char *names[4] = { "worker_a", "worker_b", "worker_c", "worker_d" };

    if (background_count() > 0)
        return 0;

    if (n < 1) n = 1;
    if (n > 4) n = 4;

    background_stop  = 0;
    background_alive = n;

    for (int i = 0; i < n; i++) {
        background_counters[i] = 0;
        task_create(names[i], entries[i]);
    }

    /* Deliberately no wait here: the point is that the shell and the GUI
     * keep responding while these run. That used to depend on preemption;
     * the workers now yield, so it still holds with the timer masked. */
    return n;
}

void stop_background(void)
{
    background_stop = 1;

    /* Give them a moment to notice and exit before reaping. */
    uint32_t until = pit_ticks() + 20;
    while (pit_ticks() < until && background_alive > 0)
        task_yield();

    for (task_t *t = task_list(); t; t = t->next)
        if (strncmp(t->name, "worker_", 7) == 0)
            task_kill(t);

    task_reap();
    background_alive = 0;
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

/* Set by the ring 3 program itself just before it asks to exit, so the test
 * can tell "ran to completion" from "died on the way". */
static volatile bool user_reached_exit;

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

    /* Record that the whole path completed. The self-test used to check only
     * that the task count returned to normal, which is true whether the task
     * exited cleanly or was killed by a fault — so it reported a pass while
     * ring 3 was in fact page faulting on its own stack. */
    user_reached_exit = true;

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

bool user_mode_completed(void)
{
    return user_reached_exit;
}

void user_mode_demo(bool use_syscall)
{
    user_wants_syscall = use_syscall;
    user_reached_exit  = false;

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

/* ---- live producer/consumer ---------------------------------------------
 *
 * Separate state from producer_consumer_run so a GUI animation and the
 * self-test cannot trip over each other. The primitives are the same:
 * two counting semaphores and a mutex. A short pause after each item is
 * what makes the buffer visible; without it the whole run finishes inside
 * one frame. */

static uint32_t live_buf[PC_BUFFER_SLOTS];
static int      live_head, live_tail;
static sem_t    live_free, live_used;
static mutex_t  live_lock;

static volatile int live_produced;
static volatile int live_consumed;
static volatile int live_occupancy;
static volatile int live_max_occ;
static volatile pc_role_state_t live_prod_st;
static volatile pc_role_state_t live_cons_st;
static volatile int live_stop;
static volatile int live_active;
static volatile int live_done;

static void live_pause(int ticks)
{
    uint32_t until = pit_ticks() + (uint32_t)ticks;
    while (pit_ticks() < until && !live_stop)
        task_yield();
}

static void live_producer(void)
{
    for (int i = 1; i <= PC_LIVE_ITEMS && !live_stop; i++) {
        live_prod_st = PC_WAIT_FULL;
        sem_wait(&live_free);
        if (live_stop) {
            sem_post(&live_free);
            break;
        }

        live_prod_st = PC_RUNNING;
        mutex_lock(&live_lock);
        live_buf[live_head] = (uint32_t)i;
        live_head = (live_head + 1) % PC_BUFFER_SLOTS;
        live_occupancy++;
        if (live_occupancy > live_max_occ)
            live_max_occ = live_occupancy;
        live_produced = i;
        mutex_unlock(&live_lock);

        sem_post(&live_used);
        /* Producer is quicker than the consumer so the buffer actually
         * fills, wraps, and shows the producer blocked on a full ring. */
        live_pause(5);
    }

    live_prod_st = PC_DONE;
    live_done++;
}

static void live_consumer(void)
{
    for (int i = 1; i <= PC_LIVE_ITEMS && !live_stop; i++) {
        live_cons_st = PC_WAIT_EMPTY;
        sem_wait(&live_used);
        if (live_stop) {
            sem_post(&live_used);
            break;
        }

        live_cons_st = PC_RUNNING;
        mutex_lock(&live_lock);
        (void)live_buf[live_tail];
        live_buf[live_tail] = 0;
        live_tail = (live_tail + 1) % PC_BUFFER_SLOTS;
        live_occupancy--;
        live_consumed = i;
        mutex_unlock(&live_lock);

        sem_post(&live_free);
        live_pause(16);
    }

    live_cons_st = PC_DONE;
    live_done++;
}

bool pc_live_running(void)
{
    return live_active && live_done < 2 && !live_stop;
}

bool pc_live_start(void)
{
    if (live_active && live_done < 2)
        return false;

    pc_live_stop();

    for (int i = 0; i < PC_BUFFER_SLOTS; i++)
        live_buf[i] = 0;

    live_head = live_tail = 0;
    live_produced = live_consumed = 0;
    live_occupancy = live_max_occ = 0;
    live_stop = 0;
    live_done = 0;
    live_prod_st = PC_RUNNING;
    live_cons_st = PC_RUNNING;

    sem_init(&live_free, PC_BUFFER_SLOTS);
    sem_init(&live_used, 0);
    mutex_init(&live_lock);

    live_active = 1;
    task_create("pc_prod", live_producer);
    task_create("pc_cons", live_consumer);
    return true;
}

void pc_live_stop(void)
{
    if (!live_active)
        return;

    live_stop = 1;

    /* Waiters sit in sem_wait. Posting both sides unblocks them so they
     * can see the stop flag; without this they would yield forever and a
     * second start would pile up tasks. */
    sem_post(&live_free);
    sem_post(&live_used);

    uint32_t until = pit_ticks() + 30;
    while (pit_ticks() < until && live_done < 2)
        task_yield();

    for (task_t *t = task_list(); t; t = t->next)
        if (strcmp(t->name, "pc_prod") == 0 || strcmp(t->name, "pc_cons") == 0)
            task_kill(t);

    task_reap();
    live_active = 0;
    live_prod_st = PC_IDLE;
    live_cons_st = PC_IDLE;
}

void pc_live_snapshot(pc_live_info_t *out)
{
    if (!out)
        return;

    preempt_disable();
    out->active     = live_active != 0;
    out->head       = live_head;
    out->tail       = live_tail;
    out->occupancy  = live_occupancy;
    out->free_count = live_free.count;
    out->used_count = live_used.count;
    out->produced   = live_produced;
    out->consumed   = live_consumed;
    out->producer   = live_active ? live_prod_st : PC_IDLE;
    out->consumer   = live_active ? live_cons_st : PC_IDLE;
    for (int i = 0; i < PC_BUFFER_SLOTS; i++)
        out->slot[i] = live_buf[i];
    /* Semaphores are only initialised when a run starts. Reporting the
     * leftover BSS zeroes as "0 free / 0 used" on the idle pane looked
     * like a full-and-empty buffer at once. */
    if (!live_active) {
        out->head = out->tail = 0;
        out->occupancy = 0;
        out->free_count = PC_BUFFER_SLOTS;
        out->used_count = 0;
    }
    preempt_enable();

    if (live_active && live_done >= 2) {
        live_active = 0;
        task_reap();
    }
}

/* ---- deadlock demonstration --------------------------------------------- */

static mutex_t dl_m1, dl_m2;
static volatile int dl_stop;
static volatile int dl_ordered;
static volatile int dl_a_done, dl_b_done;
static volatile int dl_a_holds_m1, dl_a_holds_m2, dl_a_wants_m1, dl_a_wants_m2;
static volatile int dl_b_holds_m1, dl_b_holds_m2, dl_b_wants_m1, dl_b_wants_m2;
static volatile int dl_active;

static void dl_clear_flags(void)
{
    dl_a_holds_m1 = dl_a_holds_m2 = dl_a_wants_m1 = dl_a_wants_m2 = 0;
    dl_b_holds_m1 = dl_b_holds_m2 = dl_b_wants_m1 = dl_b_wants_m2 = 0;
    dl_a_done = dl_b_done = 0;
}

static void dl_pause(int ticks)
{
    uint32_t until = pit_ticks() + (uint32_t)ticks;
    while (pit_ticks() < until && !dl_stop)
        task_yield();
}

static void dl_task_a(void)
{
    dl_a_wants_m1 = 1;
    mutex_lock(&dl_m1);
    dl_a_wants_m1 = 0;
    if (dl_stop) {
        mutex_unlock(&dl_m1);
        return;
    }
    dl_a_holds_m1 = 1;

    /* Give the other task time to take the opposite lock. Without this
     * pause, A can grab both before B runs and there is no deadlock to
     * look at. */
    dl_pause(dl_ordered ? 2 : 8);

    dl_a_wants_m2 = 1;
    mutex_lock(&dl_m2);
    dl_a_wants_m2 = 0;
    dl_a_holds_m2 = 1;

    mutex_unlock(&dl_m2);
    dl_a_holds_m2 = 0;
    mutex_unlock(&dl_m1);
    dl_a_holds_m1 = 0;
    dl_a_done = 1;
}

static void dl_task_b(void)
{
    if (dl_ordered) {
        dl_b_wants_m1 = 1;
        mutex_lock(&dl_m1);
        dl_b_wants_m1 = 0;
        if (dl_stop) {
            mutex_unlock(&dl_m1);
            return;
        }
        dl_b_holds_m1 = 1;
        dl_pause(2);
        dl_b_wants_m2 = 1;
        mutex_lock(&dl_m2);
        dl_b_wants_m2 = 0;
        dl_b_holds_m2 = 1;
        mutex_unlock(&dl_m2);
        dl_b_holds_m2 = 0;
        mutex_unlock(&dl_m1);
        dl_b_holds_m1 = 0;
    } else {
        dl_b_wants_m2 = 1;
        mutex_lock(&dl_m2);
        dl_b_wants_m2 = 0;
        if (dl_stop) {
            mutex_unlock(&dl_m2);
            return;
        }
        dl_b_holds_m2 = 1;
        dl_pause(8);
        dl_b_wants_m1 = 1;
        mutex_lock(&dl_m1);
        dl_b_wants_m1 = 0;
        dl_b_holds_m1 = 1;
        mutex_unlock(&dl_m1);
        dl_b_holds_m1 = 0;
        mutex_unlock(&dl_m2);
        dl_b_holds_m2 = 0;
    }

    dl_b_done = 1;
}

static void deadlock_reap_pair(void)
{
    for (task_t *t = task_list(); t; t = t->next)
        if (strcmp(t->name, "lock_a") == 0 || strcmp(t->name, "lock_b") == 0)
            task_kill(t);
    task_reap();
}

void deadlock_stop(void)
{
    if (!dl_active && !task_by_name("lock_a") && !task_by_name("lock_b")) {
        dl_clear_flags();
        return;
    }

    dl_stop = 1;

    /* Drop both locks so a waiter in mutex_lock can return and exit,
     * rather than spinning until the next reboot. */
    mutex_init(&dl_m1);
    mutex_init(&dl_m2);

    uint32_t until = pit_ticks() + 20;
    while (pit_ticks() < until && (!dl_a_done || !dl_b_done))
        task_yield();

    deadlock_reap_pair();
    dl_clear_flags();
    dl_active = 0;
}

void deadlock_start(bool ordered)
{
    deadlock_stop();

    mutex_init(&dl_m1);
    mutex_init(&dl_m2);
    dl_clear_flags();
    dl_stop    = 0;
    dl_ordered = ordered ? 1 : 0;
    dl_active  = 1;

    task_create("lock_a", dl_task_a);
    task_create("lock_b", dl_task_b);
}

void deadlock_kill_victim(void)
{
    if (!dl_active)
        return;

    /* A holds M1 and wants M2. Dropping M1 (and A) lets B acquire M1,
     * finish, and release M2. The kernel task is never the victim. */
    task_t *a = task_by_name("lock_a");
    if (a)
        task_kill(a);

    mutex_init(&dl_m1);
    dl_a_holds_m1 = dl_a_holds_m2 = dl_a_wants_m1 = dl_a_wants_m2 = 0;
    dl_a_done = 1;

    uint32_t until = pit_ticks() + 40;
    while (pit_ticks() < until && !dl_b_done)
        task_yield();

    deadlock_reap_pair();
    if (dl_b_done)
        dl_active = 0;
}

bool deadlock_finished(void)
{
    return dl_a_done && dl_b_done;
}

void deadlock_snapshot(deadlock_info_t *out)
{
    if (!out)
        return;

    preempt_disable();
    out->active     = dl_active != 0;
    out->ordered    = dl_ordered != 0;
    out->finished   = dl_a_done && dl_b_done;
    out->a_holds_m1 = dl_a_holds_m1 != 0;
    out->a_holds_m2 = dl_a_holds_m2 != 0;
    out->a_wants_m1 = dl_a_wants_m1 != 0;
    out->a_wants_m2 = dl_a_wants_m2 != 0;
    out->b_holds_m1 = dl_b_holds_m1 != 0;
    out->b_holds_m2 = dl_b_holds_m2 != 0;
    out->b_wants_m1 = dl_b_wants_m1 != 0;
    out->b_wants_m2 = dl_b_wants_m2 != 0;
    out->m1_owner   = dl_m1.owner_id;
    out->m2_owner   = dl_m2.owner_id;

    task_t *a = task_by_name("lock_a");
    task_t *b = task_by_name("lock_b");
    out->a_id = a ? a->id : 0;
    out->b_id = b ? b->id : 0;

    out->deadlocked = dl_active && !out->finished
                   && ((out->a_holds_m1 && out->a_wants_m2
                        && out->b_holds_m2 && out->b_wants_m1)
                    || (out->a_holds_m2 && out->a_wants_m1
                        && out->b_holds_m1 && out->b_wants_m2));
    preempt_enable();
}

/* ---- sequential vs overlapping threads ---------------------------------- */

static volatile int thread_workers_done;
static uint32_t     job_wait[THREAD_JOBS];
static uint32_t     job_spin[THREAD_JOBS];
static int          jobs_ready;

static void threads_prepare_jobs(void)
{
    calibrate_spin();

    /* One list, used by both sequential and overlapping so the comparison
     * is the same work. A little extra wait on job 0 varies between
     * comparisons because the RNG is live, not because the totals are
     * painted on. */
    for (int i = 0; i < THREAD_JOBS; i++) {
        job_wait[i] = THREAD_WAIT_TICKS;
        job_spin[i] = spins_per_tick * THREAD_COMPUTE_TICKS
                    + rng_next() % (spins_per_tick / 2 + 1);
    }
    job_wait[0] += rng_next() % 3;
    jobs_ready = 1;
}

static void thread_one_job(int i)
{
    uint32_t n;

    task_sleep(job_wait[i]);

    n = job_spin[i];
    for (volatile uint32_t d = 0; d < n; d++)
        ;
}

static void thread_jobs_range(int begin, int end)
{
    for (int i = begin; i < end; i++)
        thread_one_job(i);
}

static void thread_worker_a(void)
{
    thread_jobs_range(0, THREAD_JOBS / 2);
    thread_workers_done++;
}

static void thread_worker_b(void)
{
    thread_jobs_range(THREAD_JOBS / 2, THREAD_JOBS);
    thread_workers_done++;
}

uint32_t threads_run(bool threaded)
{
    uint32_t t0, elapsed;

    calibrate_spin();

    if (!threaded || !jobs_ready)
        threads_prepare_jobs();

    t0 = pit_ticks();

    if (!threaded) {
        thread_jobs_range(0, THREAD_JOBS);
    } else {
        thread_workers_done = 0;

        if (!task_create("thr_a", thread_worker_a)
            || !task_create("thr_b", thread_worker_b)) {
            kprintf("threads: could not create workers\n");
            return 0;
        }

        while (thread_workers_done < 2)
            task_yield();

        task_reap();
    }

    elapsed = pit_ticks() - t0;

    kprintf("threads %s: %u ticks  (%d jobs, wait %u, spin ~%u)\n",
            threaded ? "overlapping" : "sequential",
            elapsed, THREAD_JOBS,
            (uint32_t)THREAD_WAIT_TICKS, (uint32_t)THREAD_COMPUTE_TICKS);

    return elapsed;
}

/* ---- hog vs urgent job --------------------------------------------------
 *
 * The honest one-CPU story: preemption does not make the hog finish sooner.
 * It lets the short job finish while the hog is still running, instead of
 * sitting behind it. Neither task yields, so the only way the short one
 * runs mid-hog is the timer. */

static volatile int      pj_hog_done;
static volatile int      pj_urgent_done;
static volatile uint32_t pj_pair_start;
static volatile uint32_t pj_hog_start, pj_hog_end;
static volatile uint32_t pj_urgent_start, pj_urgent_end;
static bool              pj_sharing;
static bool              pj_saved_preempt;
static bool              pj_active;
static bool              pj_finished;

/* Burn this task's own CPU ticks. pit_ticks() in the inner loop would
 * measure a slower body than the hog actually runs. task->ticks is what
 * the timer accounts while we are on the CPU. */
static void burn_cpu_ticks(uint32_t want)
{
    task_t *me = task_current();
    volatile uint32_t *ticks;
    uint32_t start;
    volatile uint32_t sink = 0;

    if (!me)
        return;

    ticks = &me->ticks;
    start = *ticks;
    while (*ticks - start < want)
        sink++;
    (void)sink;
}

static void hog_body(void)
{
    pj_hog_start = pit_ticks();
    burn_cpu_ticks((uint32_t)PREEMPT_LONG_TICKS);
    pj_hog_end = pit_ticks();
    pj_hog_done = 1;
}

static void urgent_body(void)
{
    pj_urgent_start = pit_ticks();
    burn_cpu_ticks((uint32_t)PREEMPT_SHORT_TICKS);
    pj_urgent_end = pit_ticks();
    pj_urgent_done = 1;
}

void preempt_jobs_start(bool sharing)
{
    if (pj_active)
        return;

    pj_hog_done = pj_urgent_done = 0;
    pj_hog_start = pj_hog_end = 0;
    pj_urgent_start = pj_urgent_end = 0;
    pj_sharing = sharing;
    pj_saved_preempt = task_preempt_enabled();
    pj_finished = false;
    pj_active = true;

    task_set_preempt(sharing);
    pj_pair_start = pit_ticks();

    /* Hog first, then urgent. The run queue is append-only, so the first
     * yield (or the idle task noticing READY work) lands on the hog. With
     * preemption off that is the whole story: the urgent job sits until
     * the hog returns. */
    if (!task_create("hog", hog_body) || !task_create("urgent", urgent_body)) {
        kprintf("preemptjob: could not create tasks\n");
        task_set_preempt(pj_saved_preempt);
        pj_active = false;
    }
}

void preempt_jobs_poll(void)
{
    if (!pj_active)
        return;
    if (!pj_hog_done || !pj_urgent_done)
        return;

    task_reap();
    task_set_preempt(pj_saved_preempt);
    pj_active = false;
    pj_finished = true;
}

void preempt_jobs_run(bool sharing)
{
    uint32_t until;

    preempt_jobs_start(sharing);
    until = pit_ticks() + (uint32_t)PREEMPT_LONG_TICKS * 8 + 100;
    while (pj_active && pit_ticks() < until) {
        task_yield();
        preempt_jobs_poll();
    }
    preempt_jobs_poll();
}

/* ---- timed desktop hog (Notepad freeze vs type-while-busy) -------------- */

static volatile int      dh_done;
static volatile int      dh_cancel;
static volatile uint32_t dh_start, dh_end, dh_want;
static bool              dh_sharing;
static bool              dh_saved_preempt;
static bool              dh_active;
static bool              dh_finished;

static uint32_t hog_task_ticks(void)
{
    for (task_t *t = task_list(); t; t = t->next)
        if (t->state != TASK_DEAD && strcmp(t->name, "hog") == 0)
            return t->ticks;
    return 0;
}

static void desktop_hog_body(void)
{
    volatile uint32_t sink = 0;
    uint32_t until = pit_ticks() + dh_want;

    dh_start = pit_ticks();
    while ((int32_t)(until - pit_ticks()) > 0 && !dh_cancel)
        sink++;
    (void)sink;
    dh_end = pit_ticks();

    /* Restore sharing before we die. With preemption off the GUI cannot
     * run this itself until we leave the CPU. */
    task_set_preempt(dh_saved_preempt);
    kprintf("hog exited after %u ticks (sharing %s)\n",
            (dh_end > dh_start) ? dh_end - dh_start : 0,
            dh_sharing ? "on" : "OFF");
    dh_done = 1;
}

void desktop_hog_start(bool sharing, uint32_t ticks)
{
    desktop_hog_poll();
    if (dh_active)
        return;

    if (ticks < 10)
        ticks = DESKTOP_HOG_TICKS;
    if (ticks > 1200)
        ticks = 1200;   /* 12 s: the cap is a runaway guard, not the demo length */

    dh_done = 0;
    dh_cancel = 0;
    dh_start = dh_end = 0;
    dh_want = ticks;
    dh_sharing = sharing;
    dh_saved_preempt = task_preempt_enabled();
    dh_finished = false;
    dh_active = true;

    task_set_preempt(sharing);

    if (!task_create("hog", desktop_hog_body)) {
        kprintf("hog: could not create task\n");
        task_set_preempt(dh_saved_preempt);
        dh_active = false;
    }
}

void desktop_hog_stop(void)
{
    dh_cancel = 1;
}

void desktop_hog_poll(void)
{
    if (!dh_active)
        return;
    if (!dh_done)
        return;

    task_reap();
    task_set_preempt(dh_saved_preempt);
    dh_active = false;
    dh_finished = true;
}

bool desktop_hog_running(void)
{
    desktop_hog_poll();
    return dh_active;
}

void desktop_hog_snapshot(desktop_hog_info_t *out)
{
    if (!out)
        return;

    desktop_hog_poll();

    out->running       = dh_active;
    out->finished      = dh_finished;
    out->sharing       = dh_sharing;
    out->start_tick    = dh_start;
    out->end_tick      = dh_end;
    out->want_ticks    = dh_want;
    out->hog_cpu_ticks = hog_task_ticks();
    out->remain_ticks  = 0;
    if (dh_active && !dh_done) {
        uint32_t now   = pit_ticks();
        uint32_t start = dh_start ? dh_start : now;
        uint32_t until = start + dh_want;

        if ((int32_t)(until - now) > 0)
            out->remain_ticks = until - now;
    }
}

void preempt_jobs_snapshot(preempt_jobs_info_t *out)
{
    if (!out)
        return;

    preempt_jobs_poll();

    out->running     = pj_active;
    out->finished    = pj_finished;
    out->sharing     = pj_sharing;
    out->pair_start  = pj_pair_start;
    out->long_start  = pj_hog_start;
    out->long_end    = pj_hog_end;
    out->short_start = pj_urgent_start;
    out->short_end   = pj_urgent_end;
}

/* ---- two writers, one file ---------------------------------------------- */

static mutex_t        file_race_mu;
static volatile int   file_race_done;
static bool           file_race_lock;
static file_race_info_t file_race_last;

static void file_race_widen(void)
{
    /* Same idea as the numeric race: a one-byte append is too fast to lose
     * the other writer without a gap the timer can actually hit. */
    uint32_t t = pit_ticks();
    while (pit_ticks() == t)
        ;
}

static void file_race_write_record(const char *rec, uint32_t len)
{
    if (file_race_lock) {
        mutex_lock(&file_race_mu);
        fs_append(FILE_RACE_NAME, rec, len);
        mutex_unlock(&file_race_mu);
        return;
    }

    /* One byte at a time, no lock: the other cashier's characters land
     * in the middle of this line. That is a torn record, not a lost
     * increment — you can open the file and read it. */
    for (uint32_t i = 0; i < len; i++) {
        fs_append(FILE_RACE_NAME, rec + i, 1);
        file_race_widen();
    }
}

static void file_racer_a(void)
{
    static const char rec[] = "AAAAAAAA\n";
    for (int i = 0; i < FILE_RACE_LINES; i++)
        file_race_write_record(rec, sizeof(rec) - 1);
    file_race_done++;
}

static void file_racer_b(void)
{
    static const char rec[] = "BBBBBBBB\n";
    for (int i = 0; i < FILE_RACE_LINES; i++)
        file_race_write_record(rec, sizeof(rec) - 1);
    file_race_done++;
}

static void file_race_analyse(file_race_info_t *out)
{
    const fs_file_t *f = fs_find(FILE_RACE_NAME);
    uint32_t i, start;

    out->intact_a = 0;
    out->intact_b = 0;
    out->torn     = 0;
    out->size     = f ? f->size : 0;

    if (!f || !f->data) {
        out->torn = 1;
        return;
    }

    start = 0;
    for (i = 0; i <= f->size; i++) {
        if (i < f->size && f->data[i] != '\n')
            continue;

        {
            uint32_t n = i - start;
            const uint8_t *p = f->data + start;

            if (n == 0 && i == f->size)
                break;

            if (n == 8 && p[0] == 'A' && p[1] == 'A' && p[2] == 'A' && p[3] == 'A'
                && p[4] == 'A' && p[5] == 'A' && p[6] == 'A' && p[7] == 'A')
                out->intact_a++;
            else if (n == 8 && p[0] == 'B' && p[1] == 'B' && p[2] == 'B' && p[3] == 'B'
                     && p[4] == 'B' && p[5] == 'B' && p[6] == 'B' && p[7] == 'B')
                out->intact_b++;
            else
                out->torn++;
        }

        start = i + 1;
        if (i == f->size)
            break;
    }
}

void file_race_run(bool use_lock)
{
    file_race_lock = use_lock;
    file_race_done = 0;
    mutex_init(&file_race_mu);

    fs_delete(FILE_RACE_NAME);
    fs_create(FILE_RACE_NAME);

    /* Tiny appends would each rewrite the whole slot on disk. Hold that
     * until both writers finish; the bytes in RAM are already the file. */
    fs_defer_persist(true);

    if (!task_create("writer_a", file_racer_a)
        || !task_create("writer_b", file_racer_b)) {
        kprintf("filerace: could not create writers\n");
        fs_defer_persist(false);
        return;
    }

    while (file_race_done < 2)
        task_yield();

    task_reap();
    fs_defer_persist(false);
    fs_flush_file(FILE_RACE_NAME);

    file_race_last.ran    = true;
    file_race_last.locked = use_lock;
    file_race_analyse(&file_race_last);

    kprintf("till.log: %u bytes, %d clean A, %d clean B, %d torn lines\n",
            file_race_last.size, file_race_last.intact_a,
            file_race_last.intact_b, file_race_last.torn);
}

void file_race_snapshot(file_race_info_t *out)
{
    if (out)
        *out = file_race_last;
}
