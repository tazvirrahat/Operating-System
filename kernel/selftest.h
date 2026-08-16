/* selftest.h — in-kernel verification.
 *
 * Printed output on its own proves nothing: a single loop emitting "A B A B"
 * looks identical to real preemption. Each check here therefore relies on one
 * of three things that cannot be faked:
 *
 *   ablation      turn the mechanism off and show the system fails as theory
 *                 predicts (preemption off -> one task monopolises the CPU)
 *   hardware      display a value the CPU wrote, never assigned by us
 *   nondeterminism  real concurrency gives different wrong answers each run
 */
#ifndef SELFTEST_H
#define SELFTEST_H

/* Runs every check and prints a PASS/FAIL line each. Returns the number of
 * failures, so callers (and `make test`) can act on the result. */
int selftest_run(void);

#endif /* SELFTEST_H */
