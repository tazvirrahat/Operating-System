/* monitor.h — live kernel state display.
 *
 * Reads directly from the scheduler's task list and the heap's block list, so
 * what is on screen is the kernel's actual state rather than a snapshot
 * assembled for display.
 */
#ifndef MONITOR_H
#define MONITOR_H

/* Redraw once. */
void monitor_draw(void);

/* Redraw continuously until a key is pressed. */
void monitor_run(void);

#endif /* MONITOR_H */
