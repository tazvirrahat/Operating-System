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

task_t *task_current(void);
task_t *task_list(void);            /* head of the list, for iteration */
int     task_count(void);
uint32_t task_switch_count(void);

/* Free the stacks and structs of finished tasks. A task cannot do this for
 * itself while still running on the stack in question, so it happens from
 * another context. */
void    task_reap(void);

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
