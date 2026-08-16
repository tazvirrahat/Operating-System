/* sync.h — synchronisation primitives.
 *
 * The moment tasks can be preempted at arbitrary instruction boundaries,
 * any read-modify-write on shared data becomes a race: a task can be taken
 * off the CPU between reading a value and writing it back, and whatever the
 * next task does in between is lost.
 *
 * These primitives exist to close that window. The race demo in the shell
 * shows the failure first and the fix second, which is more convincing than
 * code that was simply correct from the start.
 */
#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>
#include <stdbool.h>

/* Atomically write `value` into *ptr and return the previous contents.
 * xchg carries an implicit lock prefix, so this is a single indivisible
 * bus operation — it cannot be interrupted halfway. */
static inline uint32_t atomic_xchg(volatile uint32_t *ptr, uint32_t value)
{
    __asm__ volatile ("xchg %0, %1"
                      : "+m"(*ptr), "+r"(value)
                      :
                      : "memory");
    return value;
}

/* ---- spinlock: busy-waits, never sleeps -------------------------------- */

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

void spin_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* ---- mutex: yields the CPU while waiting ------------------------------- */

typedef struct {
    volatile uint32_t locked;
    int               owner_id;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
bool mutex_try_lock(mutex_t *m);

/* ---- counting semaphore ------------------------------------------------ */

typedef struct {
    volatile int32_t count;
} sem_t;

void sem_init(sem_t *s, int32_t initial);
void sem_wait(sem_t *s);
void sem_post(sem_t *s);

#endif /* SYNC_H */
