/* pit.h — 8253/8254 Programmable Interval Timer.
 *
 * Produces a periodic interrupt on IRQ 0. This is the heartbeat that makes
 * preemptive multitasking possible: without it a task that never yields owns
 * the CPU permanently, because the kernel has no way to regain control.
 *
 * Note the dependency direction. The PIT knows nothing about tasks or the
 * scheduler; it merely offers pit_on_tick(). The scheduler registers itself.
 * That keeps the timer testable on its own and lets tests drive the scheduler
 * with a fake tick.
 */
#ifndef PIT_H
#define PIT_H

#include <stdint.h>

typedef void (*tick_callback_t)(void);

void pit_init(uint32_t hz);

/* Ticks since boot. Monotonic; wraps after ~497 days at 100 Hz. */
uint32_t pit_ticks(void);

/* Configured frequency, for converting ticks to seconds. */
uint32_t pit_hz(void);

/* Register the function called on every tick. One slot; the scheduler owns it. */
void pit_on_tick(tick_callback_t cb);

#endif /* PIT_H */
