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

/* Move every row up one and blank the last, so output can run past the
 * bottom of the screen without wrapping back to the top. */
static void scroll(void)
{
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[(y - 1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];

    for (int x = 0; x < VGA_WIDTH; x++)
        VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell(' ');

    cursor_y = VGA_HEIGHT - 1;
}

void vga_putc(char c)
{
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
