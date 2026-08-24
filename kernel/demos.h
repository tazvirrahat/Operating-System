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

/* Bounded-buffer producer/consumer.
 *
 * Two counting semaphores track free slots and filled slots; a mutex protects
 * the buffer itself. The producer blocks when the buffer is full and the
 * consumer blocks when it is empty, without either polling a flag.
 *
 * Returns true if every item was received exactly once, in order, and the
 * buffer never exceeded its capacity or went negative. */
bool producer_consumer_run(bool verbose);

/* Capacity and item count, for reporting. */
#define PC_BUFFER_SLOTS 4
#define PC_ITEM_COUNT   24

/* Spawn `n` long-running workers and return immediately, so the shell
 * stays usable while they run. Mainly so the monitor has something to show.
 *
 * Returns how many were created. Zero means a previous set is still running
 * — stacking CPU-bound tasks on top of each other is how a second `bg`
 * used to make the machine look wedged. */
int spawn_background(int n);

/* Ask the background workers to finish. */
void stop_background(void);

int      background_count(void);
uint32_t background_counter(int slot);

/* Live producer/consumer. The batch `producer_consumer_run` finishes too
 * quickly to watch; this pair run as real tasks and pause between items so
 * a GUI (or `prodcons live`) can sample the buffer each frame. The same
 * semaphores do the blocking. */
#define PC_LIVE_ITEMS 16

typedef enum {
    PC_IDLE = 0,
    PC_RUNNING,
    PC_WAIT_FULL,       /* producer, no free slot */
    PC_WAIT_EMPTY,      /* consumer, no filled slot */
    PC_DONE
} pc_role_state_t;

typedef struct {
    bool            active;
    uint32_t        slot[PC_BUFFER_SLOTS];  /* 0 = empty */
    int             head;                   /* next write */
    int             tail;                   /* next read */
    int             occupancy;
    int32_t         free_count;
    int32_t         used_count;
    int             produced;
    int             consumed;
    pc_role_state_t producer;
    pc_role_state_t consumer;
} pc_live_info_t;

bool pc_live_start(void);
void pc_live_stop(void);
bool pc_live_running(void);
void pc_live_snapshot(pc_live_info_t *out);

/* Two tasks, two mutexes. `deadlock_start` takes them in opposite order so
 * they wait for each other; `deadlock_start_ordered` takes both M1 then M2
 * and both complete. Neither call waits — the kernel task must keep running.
 * `deadlock_kill_victim` unblocks the circle by dropping one holder. */
typedef struct {
    bool active;
    bool ordered;
    bool finished;
    bool deadlocked;
    int  a_id, b_id;
    bool a_holds_m1, a_holds_m2, a_wants_m1, a_wants_m2;
    bool b_holds_m1, b_holds_m2, b_wants_m1, b_wants_m2;
    int  m1_owner, m2_owner;
} deadlock_info_t;

void deadlock_start(bool ordered);
void deadlock_stop(void);
void deadlock_kill_victim(void);
bool deadlock_finished(void);
void deadlock_snapshot(deadlock_info_t *out);

/* Spawn a task that deliberately raises a real CPU exception. The task is
 * killed by the fault handler; the kernel and shell keep running, which is
 * the point of the demonstration.
 *
 * kind: "div0"   integer division by zero      -> exception 0
 *       "opcode" the ud2 instruction           -> exception 6
 *       "gpf"    load an invalid segment       -> exception 13
 *       "null"   dereference address 0         -> exception 14
 *       "page"   dereference `addr`            -> exception 14
 *
 * The last two require paging: without an MMU every linear address is valid
 * and no dereference can fault at all. */
void fault_spawn(const char *kind, uint32_t addr);

/* Run a task in ring 3.
 *
 * use_syscall false: the task tries to write to a hardware port directly.
 *                    The CPU raises a general protection fault and the task
 *                    is killed -- unprivileged code cannot touch hardware.
 * use_syscall true:  the same task asks the kernel via int 0x80 instead,
 *                    and succeeds.
 *
 * Same code, same privilege level, different outcome, decided by hardware. */
void user_mode_demo(bool use_syscall);

/* True if the most recent ring 3 run reached its exit syscall, as opposed to
 * being killed on the way. Checking the task count alone cannot tell these
 * apart, since the task is gone either way. */
bool user_mode_completed(void);

/* Sequential vs overlapping threads.
 *
 * Each job is a real timer wait plus a calibrated spin. Sequential does
 * every job on one task. Threaded splits the same jobs across two tasks so
 * one can compute while the other is blocked. On one CPU, two pure-compute
 * threads would not finish sooner; the win is the wait. */
#define THREAD_JOBS          8
#define THREAD_WAIT_TICKS    6
#define THREAD_COMPUTE_TICKS 3

uint32_t threads_run(bool threaded);

/* Long hog + short urgent job. Neither yields. With preemption off the short
 * job waits until the hog finishes; with it on, the short job gets slices and
 * finishes while the hog is still running. The hog does not finish sooner. */
#define PREEMPT_LONG_TICKS  80
#define PREEMPT_SHORT_TICKS 8

typedef struct {
    bool     running;
    bool     finished;
    bool     sharing;           /* preemption was on for this pair */
    uint32_t pair_start;
    uint32_t long_start, long_end;
    uint32_t short_start, short_end;
} preempt_jobs_info_t;

void preempt_jobs_start(bool sharing);
void preempt_jobs_run(bool sharing);    /* start and wait, for tests/shell */
void preempt_jobs_poll(void);           /* reap and restore when both done */
void preempt_jobs_snapshot(preempt_jobs_info_t *out);

/* Two tasks appending records to the same file. Unlocked: characters from
 * the two writers land in the same lines. Locked: each record is intact. */
#define FILE_RACE_NAME  "till.log"
#define FILE_RACE_LINES 16

typedef struct {
    bool     ran;
    bool     locked;
    uint32_t size;
    int      intact_a;
    int      intact_b;
    int      torn;
} file_race_info_t;

void file_race_run(bool use_lock);
void file_race_snapshot(file_race_info_t *out);

/* Timed desktop hog. Never yields. Spins until a pit deadline, then exits
 * by itself so the machine cannot stay frozen. The caller must return to
 * its event loop — do not join. With preemption off the GUI freezes until
 * the hog dies; with it on, Notepad can take keys while the hog is listed
 * in Task Manager. */
#define DESKTOP_HOG_TICKS 1000  /* 10 s at 100 Hz.
                                 * Three seconds was long enough to see the
                                 * freeze and too short to do anything during
                                 * the sharing-on half -- by the time you had
                                 * clicked Notepad and started typing it was
                                 * already over. */

typedef struct {
    bool     running;
    bool     finished;
    bool     sharing;
    uint32_t start_tick;
    uint32_t end_tick;
    uint32_t want_ticks;
    uint32_t remain_ticks;      /* pit ticks until self-exit, 0 if idle */
    uint32_t hog_cpu_ticks;     /* task->ticks of the hog, 0 if gone */
} desktop_hog_info_t;

void desktop_hog_start(bool sharing, uint32_t ticks);
void desktop_hog_stop(void);
void desktop_hog_poll(void);
void desktop_hog_snapshot(desktop_hog_info_t *out);
bool desktop_hog_running(void);

#endif /* DEMOS_H */
