/* gfx.h — VGA graphics mode and drawing.
 *
 * Uses mode 13h: 320x200 at 256 colours, with a linear framebuffer at
 * 0xA0000 where one byte is one pixel. That linearity is the reason for
 * choosing it — the higher-resolution VGA modes are planar, and drawing a
 * single pixel there means read-modify-write across four bit planes.
 *
 * The other reason is that mode 13h can be both entered and left by writing
 * VGA registers directly. The high-resolution alternative (VESA, requested
 * from GRUB at boot) is one-way: returning to text mode needs BIOS calls that
 * are unavailable once we are in protected mode. That would have made the
 * graphical mode permanent and forced the entire existing text interface to
 * be rebuilt inside it. Here the GUI is something you enter and leave.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>

#define GFX_WIDTH  320
#define GFX_HEIGHT 200

/* Default 256-colour VGA palette entries used by the interface. */
#define C_BLACK      0
#define C_BLUE       1
#define C_GREEN      2
#define C_CYAN       3
#define C_RED        4
#define C_MAGENTA    5
#define C_BROWN      6
#define C_LGREY      7
#define C_DGREY      8
#define C_LBLUE      9
#define C_LGREEN    10
#define C_LCYAN     11
#define C_LRED      12
#define C_LMAGENTA  13
#define C_YELLOW    14
#define C_WHITE     15

/* Desktop palette, set explicitly rather than relying on the default ramp. */
#define C_DESKTOP   17
#define C_WIN_FACE  18
#define C_WIN_EDGE  19
#define C_WIN_SHADE 20
#define C_TITLE_ON  21
#define C_TITLE_OFF 22
#define C_TERM_BG   23

void gfx_enter(void);   /* switch to graphics mode */
void gfx_leave(void);   /* restore 80x25 text mode, including its font */
bool gfx_active(void);

void gfx_clear(uint8_t colour);
void gfx_pixel(int x, int y, uint8_t colour);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t colour);
void gfx_rect(int x, int y, int w, int h, uint8_t colour);
void gfx_hline(int x, int y, int w, uint8_t colour);
void gfx_vline(int x, int y, int h, uint8_t colour);

/* 8x8 bitmap text. */
#define GLYPH_W 8
#define GLYPH_H 8

void gfx_char(int x, int y, char c, uint8_t fg);
void gfx_text(int x, int y, const char *s, uint8_t fg);

/* Drawing goes to an off-screen buffer; this pushes it to the display in one
 * pass. Drawing straight to video memory produces visible tearing as windows
 * are dragged, because the CRT is reading it while we write. */
void gfx_present(void);

#endif /* GFX_H */
