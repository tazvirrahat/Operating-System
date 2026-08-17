/* shell.h — the interactive command loop.
 *
 * Runs as the kernel task. Because it blocks on the keyboard by halting the
 * CPU rather than spinning, background tasks keep running while it waits.
 */
#ifndef SHELL_H
#define SHELL_H

void shell_run(void) __attribute__((noreturn));

/* Parse and execute one command line. Exposed so the GUI's terminal window
 * can run the same commands as the text shell rather than reimplementing
 * them — there is one command table, not two. Modifies the string in place
 * while tokenising. */
void shell_dispatch(char *line);

#endif /* SHELL_H */
