/* task.h — tasks and the round-robin scheduler.
 *
 * A task is, fundamentally, a stack. Switching tasks means saving the current
 * stack pointer, loading another, and popping registers off it; execution then
 * continues wherever that task last stopped.
 *
 * Scheduling is preemptive: the PIT tick handler calls task_tick(), which
 * forces a switch when the running task's timeslice expires. Tasks are never
 * asked to cooperate and never call yield unless they want to.
 */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#define TASK_NAME_LEN 16
#define TASK_STACK_SIZE 8192

/* The boot context becomes task 1. It runs the shell and must never be killed:
 * a fault here is unrecoverable, whereas a fault in any other task simply
 * terminates that task. */
#define KERNEL_TASK_ID 1

/* Sleep until this tick, or until task_idle_nudge(). Used by the GUI when
 * the scene is idle so the dedicated idle task can halt the CPU. */
#define TASK_SLEEP_FOREVER 0xFFFFFFFFu

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD,
} task_state_t;

typedef struct task {
    uint32_t     esp;                   /* saved stack pointer while suspended */
    uint32_t     stack_base;            /* lowest address of the stack region */
    uint32_t     stack_size;
    int          id;
    char         name[TASK_NAME_LEN];
    task_state_t state;
    uint32_t     ticks;                 /* CPU time received, in timer ticks */
    uint32_t     wake_tick;             /* 0 = not sleeping; else unblock at this pit tick */
    struct task *next;
} task_t;

typedef void (*task_entry_t)(void);

void    task_init(void);
task_t *task_create(const char *name, task_entry_t entry);

/* Called from the timer interrupt. Accounts CPU time and preempts when the
 * timeslice runs out. */
void    task_tick(void);

/* Give up the rest of this timeslice voluntarily. */
void    task_yield(void);

/* Terminate the calling task. Never returns. A task's entry function
 * returning normally lands here too. */
void    task_exit(void) __attribute__((noreturn));

/* Block / wake, used by the synchronisation primitives. */
void    task_block(void);
void    task_unblock(task_t *t);

/* Park the caller until wake_tick (or forever) or an input IRQ. Unlike
 * yield, the caller is not READY, so the idle task can halt. The deadline
 * is the same wake_tick walker as task_sleep(); mouse and keyboard still
 * call task_idle_nudge() so a click does not wait for the next panel. */
void    task_idle_wait(uint32_t wake_tick);
void    task_idle_nudge(void);

/* Block until `ticks` timer interrupts have elapsed. Other READY tasks run
 * in the meantime, which is how a waiter overlaps with someone else's
 * compute on a single CPU. `hlt` is not enough: it still leaves the caller
 * READY, so pick_next would come straight back. */
void    task_sleep(uint32_t ticks);

/* True if some non-idle task other than the caller is READY. The GUI yields
 * to those rather than parking, so workers keep the CPU. */
bool    task_others_ready(void);

int     task_idle_id(void);
bool    task_is_idle(const task_t *t);

task_t *task_current(void);
task_t *task_list(void);            /* head of the list, for iteration */
int     task_count(void);
uint32_t task_switch_count(void);

/* Free the stacks and structs of finished tasks. A task cannot do this for
 * itself while still running on the stack in question, so it happens from
 * another context. */
void    task_reap(void);

/* Mark a task dead from the outside. Used to recover a deadlock demonstration
 * without rebooting: the victim is skipped by the scheduler and reaped, and
 * the kernel task is refused so a mis-click cannot take down the shell. */
void    task_kill(task_t *t);

task_t *task_by_id(int id);
task_t *task_by_name(const char *name);

/* Recent CPU owners, one sample per timer tick. Written from the tick path
 * with interrupts already off, so the store is the whole critical section.
 * Length is a power of two so the index is a mask rather than a divide. */
#define SCHED_TRACE_LEN 256

uint32_t task_sched_trace(uint32_t *ids, uint32_t max);

/* Short critical sections that must not be split across a context switch.
 *
 * Nests, so a caller need not know whether an outer section is already open.
 * Interrupts stay enabled throughout — the timer keeps ticking and time is
 * still accounted, only the reschedule is deferred. Use this for regions
 * measured in microseconds; anything longer belongs behind a mutex.
 */
void    preempt_disable(void);
void    preempt_enable(void);

/* Preemption ablation. Masking preemption should visibly break scheduling:
 * with it off, the running task keeps the CPU indefinitely. That failure is
 * the proof that preemption was doing the work in the first place. */
void    task_set_preempt(bool enabled);
bool    task_preempt_enabled(void);

/* Stack overflow detection. Each stack carries a guard word at its lowest
 * address; if a task's stack grows far enough to overwrite it, the corruption
 * is reported instead of silently damaging another task. */
bool    task_check_stacks(task_t **culprit);

const char *task_state_name(task_state_t s);

#endif /* TASK_H */
