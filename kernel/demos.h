/* demos.h — the demonstrations that prove each subsystem works.
 *
 * These live apart from the shell because the self-test drives exactly the
 * same code. A demo the user watches and an assertion the machine checks
 * should not be two separate implementations that can drift apart.
 */
#ifndef DEMOS_H
#define DEMOS_H

#include <stdint.h>
#include <stdbool.h>

/* Number of increments each racer performs. The expected total is twice this.
 *
 * Kept small because each iteration deliberately spans a timer tick (see the
 * comment in racer()), so the count is bounded by how long we are willing to
 * wait rather than by CPU speed. 50 iterations is about half a second per run. */
#define RACE_ITERATIONS 50
#define RACE_EXPECTED   (RACE_ITERATIONS * 2)

/* Run two tasks that both increment a shared counter RACE_ITERATIONS times.
 * With use_lock false the read-modify-write is unprotected and the total comes
 * out low and differently wrong on every run; with it true the total is exact.
 * Returns the final counter value. */
uint32_t race_run(bool use_lock);

/* Spawn `n` tasks that spin in tight loops printing their own letter. They
 * never yield, so any interleaving is caused by timer preemption. Blocks until
 * they finish. */
void spawn_printers(int n);

/* Start a task that produces no output and only increments a counter, so that
 * evidence of it having run cannot come from anything it printed. */
void quiet_start(void);
uint32_t quiet_stop_and_read(void);

/* Spawn a task that deliberately raises a real CPU exception. The task is
 * killed by the fault handler; the kernel and shell keep running, which is
 * the point of the demonstration.
 *
 * kind: "div0"   integer division by zero      -> exception 0
 *       "opcode" the ud2 instruction           -> exception 6
 *       "gpf"    load an invalid segment       -> exception 13
 *
 * Note there is no "pagefault" option: without paging enabled every linear
 * address is valid, so no dereference can fault. That demo arrives with the
 * MMU. */
void fault_spawn(const char *kind);

#endif /* DEMOS_H */
