/* gui.h — a small windowing environment.
 *
 * Draggable windows with title bars, a mouse cursor, and a terminal window
 * that runs the same commands as the text shell. Entered from the shell with
 * `gui` and left with Escape, which restores the text console exactly as it
 * was — nothing about the text interface is replaced or rewritten.
 *
 * This is deliberately application code sitting on top of the kernel rather
 * than part of it. The operating system content is underneath: the scheduler
 * that keeps background tasks running while windows are dragged, the mouse
 * and keyboard interrupt handlers feeding the event loop, and the heap the
 * whole thing allocates from.
 */
#ifndef GUI_H
#define GUI_H

void gui_run(void);

#endif /* GUI_H */
