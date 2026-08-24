#include "task.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "isr.h"
#include "pit.h"

/* Written at the lowest address of every task stack. x86 stacks grow down, so
 * this is the first thing an overflowing stack destroys. */
#define STACK_GUARD 0xDEADC0DE

/* Timer ticks a task gets before it is preempted. One tick at 100 Hz is 10 ms.
 *
 * A longer slice reduces switching overhead, but overhead is irrelevant at
 * this scale and a short slice makes the behaviour far easier to observe:
 * output interleaves finely, and critical sections a few milliseconds wide
 * are reliably interrupted rather than sailing through untouched. */
#define TIMESLICE 1

/* Defined in context.asm. */
extern void context_switch(uint32_t *old_esp_out, uint32_t new_esp);
extern void context_start(uint32_t new_esp);

static task_t  *current;
static task_t  *tasks;              /* singly linked list, never empty once init'd */
static task_t  *idle_task;
static task_t  *idle_sleeper;
static int      next_id = 1;
static int      slice_remaining;
static bool     preempt = true;
static uint32_t switches;

/* Nesting depth of preempt_disable(). Non-zero defers rescheduling. */
static volatile int preempt_depth;

/* Who had the CPU on each of the last SCHED_TRACE_LEN ticks. Written only
 * from task_tick, which already runs with interrupts off. */
static volatile uint32_t sched_trace[SCHED_TRACE_LEN];
static volatile uint32_t sched_trace_head;

const char *task_state_name(task_state_t s)
{
    switch (s) {
    case TASK_READY:   return "READY";
    case TASK_RUNNING: return "RUNNING";
    case TASK_BLOCKED: return "BLOCKED";
    case TASK_DEAD:    return "DEAD";
    }
    return "?";
}

static bool is_idle_task(const task_t *t)
{
    return idle_task && t == idle_task;
}

bool task_is_idle(const task_t *t)
{
    return is_idle_task(t);
}

int task_idle_id(void)
{
    return idle_task ? idle_task->id : 0;
}

bool task_others_ready(void)
{
    for (task_t *t = tasks; t; t = t->next) {
        if (t == current || is_idle_task(t))
            continue;
        if (t->state == TASK_READY || t->state == TASK_RUNNING)
            return true;
    }
    return false;
}

/* Halt, then give the CPU to anyone who became READY while we slept.
 *
 * Yielding *before* hlt was a livelock: if the waiter was already READY
 * (a nudge racing the switch into this task) we spun between idle and the
 * GUI at full speed, which is how Task Manager still read 96% kernel with
 * tens of thousands of switches a second. HLT first caps the rate at the
 * IRQ rate. Mouse and keyboard IRQs still wake us, so input latency stays
 * one interrupt rather than a timeslice.
 *
 * This task is skipped by pick_next whenever anyone else is READY, so it
 * cannot steal the CPU from workers and cannot run during the preempt-off
 * ablation (those tasks never yield, so they never reach us). */
static void idle_body(void)
{
    for (;;) {
        __asm__ volatile ("sti; hlt");
        if (task_others_ready())
            task_yield();
    }
}

void task_init(void)
{
    /* The context we are already running in becomes task 1. Its esp is not
     * set here: the first context_switch away from it will record where it
     * actually stopped. */
    task_t *t = kmalloc(sizeof(task_t));
    if (!t)
        panic("task_init: out of memory");

    memset(t, 0, sizeof(*t));
    strncpy(t->name, "kernel", TASK_NAME_LEN - 1);
    t->id         = next_id++;
    t->state      = TASK_RUNNING;
    t->stack_base = 0;              /* the boot stack, not heap allocated */
    t->stack_size = 0;
    t->next       = 0;

    tasks   = t;
    current = t;
    slice_remaining = TIMESLICE;

    idle_task = task_create("System idle", idle_body);
    if (!idle_task)
        panic("task_init: no idle task");

    kprintf("scheduler        : round-robin, %d tick timeslice, preemptive\n",
            TIMESLICE);
}

task_t *task_create(const char *name, task_entry_t entry)
{
    task_t *t = kmalloc(sizeof(task_t));
    if (!t)
        return 0;

    uint8_t *stack = kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        kfree(t);
        return 0;
    }

    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->stack_base = (uint32_t)stack;
    t->stack_size = TASK_STACK_SIZE;

    *(uint32_t *)stack = STACK_GUARD;

    /* Build a stack that looks exactly as if this task had been suspended
     * inside context_switch. The pops there will then walk off this frame and
     * `ret` straight into the entry point.
     *
     * Layout, from the top of the stack downward:
     *     task_exit      <- where entry returns to, if it ever returns
     *     entry          <- what the final `ret` pops into eip
     *     ebp, ebx, esi, edi
     *     eflags         <- what popfd reads; esp points here
     */
    uint32_t *sp = (uint32_t *)(t->stack_base + t->stack_size);

    *--sp = (uint32_t)task_exit;    /* safety net for a task that returns */
    *--sp = (uint32_t)entry;
    *--sp = 0;                      /* ebp */
    *--sp = 0;                      /* ebx */
    *--sp = 0;                      /* esi */
    *--sp = 0;                      /* edi */
    *--sp = 0x202;                  /* eflags: reserved bit 1, IF set */

    t->esp = (uint32_t)sp;

    /* Append to the list. Interrupts are masked because the timer handler
     * walks this same list. */
    bool were_on = interrupts_enabled();
    cli();

    task_t *last = tasks;
    while (last->next)
        last = last->next;
    last->next = t;

    if (were_on)
        sti();

    return t;
}

/* Round robin among real work. The idle task is READY from the moment it is
 * created, but it is only selected when nobody else is — otherwise the first
 * timer tick after boot would steal the CPU from kmain, and a yield from the
 * GUI would spin between kernel and idle at full speed, which is the bug
 * this task exists to end. */
static task_t *pick_next(void)
{
    task_t *start = current ? current->next : tasks;
    task_t *idle_ready = 0;

    for (int wrapped = 0; wrapped < 2; wrapped++) {
        for (task_t *t = start ? start : tasks; t; t = t->next) {
            if (t->state != TASK_READY)
                continue;
            if (is_idle_task(t)) {
                idle_ready = t;
                continue;
            }
            return t;
        }
        start = tasks;
    }

    if (idle_ready)
        return idle_ready;

    /* Nothing else runnable. Stay put if we still can. */
    if (current && (current->state == TASK_RUNNING || current->state == TASK_READY))
        return current;

    return idle_task;
}

static void reap_dead(void)
{
    task_t *prev = 0;

    for (task_t *t = tasks; t; ) {
        task_t *next = t->next;

        if (t->state == TASK_DEAD && t != current) {
            if (prev)
                prev->next = next;
            else
                tasks = next;

            if (t->stack_base)
                kfree((void *)t->stack_base);
            kfree(t);

            t = next;
            continue;
        }

        prev = t;
        t = next;
    }
}

/* The actual switch. Must be called with interrupts disabled.
 *
 * The caller is marked READY first (if it was RUNNING) so pick_next can see
 * it as a candidate. A waiter that already set BLOCKED stays blocked, which
 * is how the GUI parks itself so idle can halt. */
static void schedule(void)
{
    task_t *prev = current;
    if (!prev)
        return;

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;

    task_t *next = pick_next();
    if (!next)
        next = prev;

    if (next == prev) {
        if (prev->state == TASK_READY)
            prev->state = TASK_RUNNING;
        return;
    }

    next->state = TASK_RUNNING;
    current     = next;
    switches++;
    slice_remaining = TIMESLICE;

    context_switch(&prev->esp, next->esp);

    /* Execution resumes here whenever this task is scheduled again — possibly
     * a long time later, and on a different stack than the caller expects. */
}

void preempt_disable(void)
{
    /* No lock needed: a context switch can only happen through this same
     * counter, and an interrupt landing between the read and the write would
     * still leave the value consistent because nothing else writes it. */
    preempt_depth++;
}

void preempt_enable(void)
{
    if (preempt_depth > 0)
        preempt_depth--;
}

void task_idle_nudge(void)
{
    if (!idle_sleeper)
        return;

    idle_sleeper->wake_tick = 0;
    if (idle_sleeper->state == TASK_BLOCKED)
        idle_sleeper->state = TASK_READY;
    idle_sleeper = 0;
}

void task_idle_wait(uint32_t wake_tick)
{
    bool were_on = interrupts_enabled();
    cli();

    /* Same BLOCKED + wake_tick shape as task_sleep, plus idle_sleeper so
     * an input IRQ can unblock us before the deadline. A wait that only
     * recorded idle_wake_tick (and not current->wake_tick) still woke from
     * the timer — but a click that arrived while we were drawing was then
     * parked until that deadline, because the waiter is not registered
     * during the blit. The GUI now skips the wait when input is already
     * queued; this side is so a deadline cannot be forgotten. */
    idle_sleeper = current;
    current->wake_tick = (wake_tick == TASK_SLEEP_FOREVER) ? 0 : wake_tick;
    current->state  = TASK_BLOCKED;
    slice_remaining = TIMESLICE;
    schedule();

    current->wake_tick = 0;
    idle_sleeper = 0;

    if (were_on)
        sti();
}

void task_sleep(uint32_t ticks)
{
    if (!ticks || !current)
        return;

    bool were_on = interrupts_enabled();
    cli();

    current->wake_tick  = pit_ticks() + ticks;
    current->state      = TASK_BLOCKED;
    slice_remaining     = TIMESLICE;
    schedule();

    if (were_on)
        sti();
}

void task_tick(void)
{
    if (!current)
        return;

    /* Time is accounted even inside a critical section — only the reschedule
     * is deferred, so a task cannot hide its CPU usage by deferring. */
    current->ticks++;
    sched_trace[sched_trace_head & (SCHED_TRACE_LEN - 1)] = (uint32_t)current->id;
    sched_trace_head++;

    /* Anyone blocked on a tick: GUI idle-wait, task_sleep, both. Input IRQs
     * still nudge the GUI sleeper on their own so a click does not wait
     * for this deadline. */
    {
        uint32_t now = pit_ticks();

        for (task_t *t = tasks; t; t = t->next) {
            if (t->state == TASK_BLOCKED && t->wake_tick != 0
                && now >= t->wake_tick) {
                t->wake_tick = 0;
                t->state = TASK_READY;
                if (t == idle_sleeper)
                    idle_sleeper = 0;
            }
        }
    }

    /* Ablation switch. With preemption off the running task is never taken
     * off the CPU, which is exactly the failure the demo shows. */
    if (!preempt)
        return;

    /* Someone is midway through something that must not be split. Let the
     * slice run over rather than switch; they will be along shortly. */
    if (preempt_depth > 0)
        return;

    if (--slice_remaining > 0)
        return;

    slice_remaining = TIMESLICE;
    schedule();
}

void task_yield(void)
{
    bool were_on = interrupts_enabled();
    cli();

    slice_remaining = TIMESLICE;
    schedule();

    if (were_on)
        sti();
}

void task_exit(void)
{
    cli();

    current->state = TASK_DEAD;

    /* Do not free our own stack here — we are still standing on it. The next
     * scheduler pass reaps it from another task's context. */
    schedule();

    /* schedule() never returns to a dead task, but if every other task is also
     * gone there is nothing to switch to. Halt rather than fall through. */
    for (;;)
        __asm__ volatile ("hlt");
}

void task_block(void)
{
    bool were_on = interrupts_enabled();
    cli();

    current->state = TASK_BLOCKED;
    schedule();

    if (were_on)
        sti();
}

void task_unblock(task_t *t)
{
    if (t && t->state == TASK_BLOCKED)
        t->state = TASK_READY;
}

task_t *task_current(void) { return current; }
task_t *task_list(void)    { return tasks; }

int task_count(void)
{
    int n = 0;
    for (task_t *t = tasks; t; t = t->next)
        n++;
    return n;
}

uint32_t task_switch_count(void) { return switches; }

void task_set_preempt(bool enabled)
{
    preempt = enabled;
    slice_remaining = TIMESLICE;
}

bool task_preempt_enabled(void) { return preempt; }

bool task_check_stacks(task_t **culprit)
{
    for (task_t *t = tasks; t; t = t->next) {
        if (!t->stack_base)
            continue;   /* the boot stack has no guard */

        if (*(uint32_t *)t->stack_base != STACK_GUARD) {
            if (culprit)
                *culprit = t;
            return false;
        }
    }

    if (culprit)
        *culprit = 0;

    return true;
}

void task_reap(void)
{
    bool were_on = interrupts_enabled();
    cli();
    reap_dead();
    if (were_on)
        sti();
}

void task_kill(task_t *t)
{
    if (!t || t->id == KERNEL_TASK_ID || t == current || is_idle_task(t))
        return;

    bool were_on = interrupts_enabled();
    cli();
    if (t->state != TASK_DEAD) {
        t->wake_tick = 0;
        t->state = TASK_DEAD;
    }
    if (were_on)
        sti();
}

task_t *task_by_id(int id)
{
    for (task_t *t = tasks; t; t = t->next)
        if (t->id == id)
            return t;
    return 0;
}

task_t *task_by_name(const char *name)
{
    for (task_t *t = tasks; t; t = t->next)
        if (strcmp(t->name, name) == 0)
            return t;
    return 0;
}

uint32_t task_sched_trace(uint32_t *ids, uint32_t max)
{
    if (!ids || !max)
        return 0;

    uint32_t head = sched_trace_head;
    uint32_t n = head < SCHED_TRACE_LEN ? head : SCHED_TRACE_LEN;
    if (n > max)
        n = max;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (head - n + i) & (SCHED_TRACE_LEN - 1);
        ids[i] = sched_trace[idx];
    }

    return n;
}
