/* vga.h — VGA text mode driver.
 *
 * The screen is memory-mapped at 0xB8000 as an 80x25 grid of 16-bit cells:
 * low byte is the character, high byte is (background << 4 | foreground).
 * No driver initialisation is needed — the memory is simply there.
 */
#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stdbool.h>

enum vga_color {
    VGA_BLACK = 0,  VGA_BLUE = 1,       VGA_GREEN = 2,      VGA_CYAN = 3,
    VGA_RED = 4,    VGA_MAGENTA = 5,    VGA_BROWN = 6,      VGA_LGREY = 7,
    VGA_DGREY = 8,  VGA_LBLUE = 9,      VGA_LGREEN = 10,    VGA_LCYAN = 11,
    VGA_LRED = 12,  VGA_LMAGENTA = 13,  VGA_YELLOW = 14,    VGA_WHITE = 15,
};

void vga_init(void);
void vga_putc(char c);
void vga_clear(void);

/* Move the cursor back to the top-left without erasing anything, so the next
 * redraw overwrites the previous one in place. */
void vga_home(void);

/* Scrollback.
 *
 * Lines pushed off the top of the screen are kept in a ring buffer so they can
 * be read again. Without this, anything longer than 25 lines -- the self-test,
 * a fault dump -- is simply gone, which is awkward when the whole point is to
 * show someone what happened.
 *
 * Scrolling is a display concern only: it never changes what has been written,
 * and any new output snaps the view back to the bottom so live output is never
 * silently hidden.
 */
void vga_scroll_back(int lines);    /* positive = further into the past */
void vga_scroll_to_bottom(void);
bool vga_is_scrolled(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_move_cursor(uint8_t x, uint8_t y);

/* Current column. Lets a caller pad a line to an exact width without having
 * to count what it has already printed, which is how the monitor keeps its
 * box borders aligned. */
uint8_t vga_get_x(void);

#endif /* VGA_H */
