#include "sync.h"
#include "task.h"
#include "isr.h"    /* cli / sti / interrupts_enabled */

void spin_init(spinlock_t *lock)
{
    lock->locked = 0;
}

void spin_lock(spinlock_t *lock)
{
    /* Keep swapping 1 in until we get 0 back, meaning it was previously
     * unlocked and we are now the holder. */
    while (atomic_xchg(&lock->locked, 1) != 0)
        __asm__ volatile ("pause");   /* hint to the CPU that this is a spin */
}

void spin_unlock(spinlock_t *lock)
{
    atomic_xchg(&lock->locked, 0);
}

void mutex_init(mutex_t *m)
{
    m->locked   = 0;
    m->owner_id = 0;
}

bool mutex_try_lock(mutex_t *m)
{
    if (atomic_xchg(&m->locked, 1) == 0) {
        task_t *self = task_current();
        m->owner_id = self ? self->id : 0;
        return true;
    }

    return false;
}

void mutex_lock(mutex_t *m)
{
    /* Unlike a spinlock, give up the rest of the timeslice on each failed
     * attempt. On a single CPU the holder cannot make progress while we are
     * spinning, so yielding is what actually lets the lock become free. */
    while (!mutex_try_lock(m))
        task_yield();
}

void mutex_unlock(mutex_t *m)
{
    m->owner_id = 0;
    atomic_xchg(&m->locked, 0);
}

void sem_init(sem_t *s, int32_t initial)
{
    s->count = initial;
}

void sem_wait(sem_t *s)
{
    for (;;) {
        /* Interrupts off makes the test-and-decrement atomic with respect to
         * the scheduler, which is the only source of concurrency here. */
        bool were_on = interrupts_enabled();
        cli();

        if (s->count > 0) {
            s->count--;
            if (were_on)
                sti();
            return;
        }

        if (were_on)
            sti();

        task_yield();
    }
}

void sem_post(sem_t *s)
{
    bool were_on = interrupts_enabled();
    cli();

    s->count++;

    if (were_on)
        sti();
}
