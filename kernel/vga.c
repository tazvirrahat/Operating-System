#include "vga.h"
#include "io.h"

#define VGA_MEM    ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* CRT controller ports, used to move the blinking hardware cursor. */
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

static uint8_t cursor_x;
static uint8_t cursor_y;
static uint8_t color = (VGA_BLACK << 4) | VGA_LGREY;

static uint16_t cell(char c)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    color = (uint8_t)((bg << 4) | fg);
}

void vga_move_cursor(uint8_t x, uint8_t y)
{
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);

    outb(CRTC_INDEX, 0x0F);
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(CRTC_INDEX, 0x0E);
    outb(CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = cell(' ');

    cursor_x = 0;
    cursor_y = 0;
    vga_move_cursor(0, 0);
}

/* ---- scrollback ---------------------------------------------------------
 *
 * A ring of lines that have been pushed off the top. Rendering while scrolled
 * treats history followed by the live screen as one continuous buffer and
 * shows a 25-line window into it.
 *
 * 512 lines is about 80 KB, which is nothing against the 128 MB the machine
 * reports, and is roughly twenty screens -- comfortably more than the longest
 * thing the kernel prints.
 */
#define HISTORY_LINES 512

static uint16_t history[HISTORY_LINES][VGA_WIDTH];
static int      history_count;      /* lines stored, saturating */
static int      history_next;       /* write position (circular) */

/* Snapshot of the live screen, taken when scrolling begins so it can be put
 * back exactly. Scrolling must not disturb what was on screen. */
static uint16_t live_snapshot[VGA_HEIGHT][VGA_WIDTH];
static int      scroll_offset;      /* 0 = live view */

static void history_push(const volatile uint16_t *line)
{
    for (int x = 0; x < VGA_WIDTH; x++)
        history[history_next][x] = line[x];

    history_next = (history_next + 1) % HISTORY_LINES;
    if (history_count < HISTORY_LINES)
        history_count++;
}

/* Fetch a line from the combined history + snapshot buffer.
 * Index 0 is the oldest line still kept. */
static const uint16_t *combined_line(int index)
{
    if (index < history_count) {
        int slot = (history_next - history_count + index + HISTORY_LINES * 2)
                 % HISTORY_LINES;
        return history[slot];
    }

    return live_snapshot[index - history_count];
}

static void render_scrolled(void)
{
    int total = history_count + VGA_HEIGHT;
    int top   = total - VGA_HEIGHT - scroll_offset;

    if (top < 0)
        top = 0;

    for (int y = 0; y < VGA_HEIGHT; y++) {
        const uint16_t *src = combined_line(top + y);
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[y * VGA_WIDTH + x] = src[x];
    }
}

void vga_scroll_back(int lines)
{
    if (history_count == 0)
        return;

    /* Entering scroll mode: preserve the live screen before overwriting it. */
    if (scroll_offset == 0) {
        for (int y = 0; y < VGA_HEIGHT; y++)
            for (int x = 0; x < VGA_WIDTH; x++)
                live_snapshot[y][x] = VGA_MEM[y * VGA_WIDTH + x];
    }

    scroll_offset += lines;

    if (scroll_offset > history_count)
        scroll_offset = history_count;
    if (scroll_offset < 0)
        scroll_offset = 0;

    if (scroll_offset == 0) {
        vga_scroll_to_bottom();
        return;
    }

    render_scrolled();
}

void vga_scroll_to_bottom(void)
{
    if (scroll_offset == 0)
        return;

    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[y * VGA_WIDTH + x] = live_snapshot[y][x];

    scroll_offset = 0;
    vga_move_cursor(cursor_x, cursor_y);
}

bool vga_is_scrolled(void)
{
    return scroll_offset != 0;
}

/* Move every row up one and blank the last, so output can run past the
 * bottom of the screen without wrapping back to the top. The line leaving
 * the top is kept. */
static void scroll(void)
{
    history_push(&VGA_MEM[0]);

    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[(y - 1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];

    for (int x = 0; x < VGA_WIDTH; x++)
        VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell(' ');

    cursor_y = VGA_HEIGHT - 1;
}

void vga_putc(char c)
{
    /* New output always returns to the live view. Writing into a screen the
     * user has scrolled away from would either corrupt the history they are
     * reading or hide the output entirely. */
    if (scroll_offset != 0)
        vga_scroll_to_bottom();

    switch (c) {
    case '\n':
        cursor_x = 0;
        cursor_y++;
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\t':
        cursor_x = (uint8_t)((cursor_x + 4) & ~3);
        break;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
            VGA_MEM[cursor_y * VGA_WIDTH + cursor_x] = cell(' ');
        }
        break;
    default:
        VGA_MEM[cursor_y * VGA_WIDTH + cursor_x] = cell(c);
        cursor_x++;
        break;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }

    if (cursor_y >= VGA_HEIGHT)
        scroll();

    vga_move_cursor(cursor_x, cursor_y);
}

uint8_t vga_get_x(void)
{
    return cursor_x;
}

void vga_home(void)
{
    cursor_x = 0;
    cursor_y = 0;
    vga_move_cursor(0, 0);
}

void vga_init(void)
{
    vga_set_color(VGA_LGREY, VGA_BLACK);
    vga_clear();
}
