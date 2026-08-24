/* fb.h — linear framebuffer graphics.
 *
 * GRUB is asked at boot to set a high-resolution mode and reports back the
 * address of a linear framebuffer: one 32-bit value per pixel, laid out in
 * rows. From there this is pure software rendering. There is no GPU driver
 * and no acceleration of any kind — every pixel is computed and written by
 * the CPU.
 *
 * That is why the dirty-rectangle tracking below matters rather than being an
 * optimisation to add later. At 1920x1080x32 a full frame is 8.3 MB, and
 * copying that to video memory on every update is far too slow to feel
 * interactive. Instead only the region that actually changed is copied:
 * moving the mouse touches a few hundred bytes rather than eight megabytes.
 */
#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stdbool.h>

#include "multiboot.h"

/* Returns false if GRUB could not give us a graphics mode, in which case the
 * kernel stays in VGA text mode and the GUI is unavailable. */
bool fb_init(const multiboot_info_t *mb);

bool     fb_available(void);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_phys_addr(void);
uint32_t fb_nbytes(void);

/* Pack 8-bit components into the framebuffer's pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_clear(uint32_t colour);
void fb_pixel(int x, int y, uint32_t colour);
void fb_fill_rect(int x, int y, int w, int h, uint32_t colour);
void fb_rect(int x, int y, int w, int h, uint32_t colour);

/* Text, using the 8x8 font scaled by an integer factor. At 1920x1080 an
 * unscaled 8x8 glyph is close to unreadable, so the console uses a scale of
 * 2 and the result is equivalent to a 16x16 font. */
void fb_char(int x, int y, char c, uint32_t fg, int scale);
void fb_text(int x, int y, const char *s, uint32_t fg, int scale);

/* Mark a region as changed. Drawing calls do this themselves; it is exposed
 * for callers that write to the backbuffer directly. */
void fb_mark_dirty(int x, int y, int w, int h);

/* Copy the dirty region to the display and clear the record. */
void fb_present(void);

/* Copy one rectangle from the backbuffer to the front, without touching the
 * dirty tracker. Used when several disjoint windows changed: presenting their
 * bounding box would copy the wallpaper between them. */
void fb_copy_rect(int x, int y, int w, int h);

/* Drop the recorded dirty region without copying. Pair with fb_copy_rect when
 * the caller has already pushed the pixels it cares about. */
void fb_reset_dirty(void);

/* Read a rectangle out of the backbuffer, and write one back.
 *
 * These exist for the mouse cursor. Drawing a cursor by repainting the whole
 * scene underneath it costs a full-screen redraw for every pixel of mouse
 * movement. Saving what is beneath it instead, restoring that when it moves,
 * and blitting it at the new position touches two small rectangles rather
 * than the entire display -- which at 1920x1080 is the difference between
 * copying a few kilobytes and copying 8.3 megabytes.
 *
 * The caller supplies the storage and is responsible for its size: w * h
 * pixels of 32 bits each.
 */
void fb_read_rect(int x, int y, int w, int h, uint32_t *dst);
void fb_write_rect(int x, int y, int w, int h, const uint32_t *src);

/* Force the whole screen to be copied on the next present. */
void fb_mark_all_dirty(void);


/* ---- anti-aliased text ----
 *
 * Glyphs are blended by coverage rather than drawn as on/off pixels, which is
 * what gives smooth edges. `ui` selects the proportional interface face; false
 * selects the monospaced one used by the terminal.
 *
 * fb_char_aa returns the advance width, which for a proportional face is not
 * the cell width -- an 'i' advances less than a 'W'.
 */
int  fb_char_aa(int x, int y, char c, uint32_t colour, bool ui);
int  fb_text_aa(int x, int y, const char *s, uint32_t colour, bool ui);
int  fb_text_width(const char *s, bool ui);
int  fb_font_height(bool ui);
int  fb_mono_advance(void);

/* Blend a colour over a region. Used for shadows and translucent panels. */
void fb_blend_rect(int x, int y, int w, int h, uint32_t colour, uint32_t alpha);


/* Rounded rectangle with anti-aliased corners, and a soft drop shadow.
 * Both are alpha operations: omitting corner pixels outright gives a hard
 * stair-stepped arc that looks worse than a square corner. */
void fb_fill_round_rect(int x, int y, int w, int h, int radius, uint32_t colour);
void fb_shadow(int x, int y, int w, int h, int radius, int spread);

void fb_blend_round_rect(int x, int y, int w, int h, int radius,
                         uint32_t colour, uint32_t alpha);

#endif
