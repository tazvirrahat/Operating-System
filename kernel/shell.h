/* shell.h — the interactive command loop.
 *
 * Runs as the kernel task. Because it blocks on the keyboard by halting the
 * CPU rather than spinning, background tasks keep running while it waits.
 */
#ifndef SHELL_H
#define SHELL_H

void shell_run(void) __attribute__((noreturn));

#endif /* SHELL_H */
