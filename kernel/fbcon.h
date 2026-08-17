/* fbcon.h — text console rendered into the framebuffer.
 *
 * Once GRUB puts the machine in a graphics mode, the VGA text buffer at
 * 0xB8000 no longer displays anything. Everything the kernel prints — boot
 * messages, the self-test, fault dumps — has to be drawn as pixels instead.
 *
 * This keeps a grid of characters and renders them, so the console behaves
 * exactly as it did in text mode from the point of view of every caller.
 * kprintf is unchanged and unaware of which one it is talking to.
 */
#ifndef FBCON_H
#define FBCON_H

#include <stdint.h>
#include <stdbool.h>

/* Scale applied to the 8x8 font. At 1920x1080 an unscaled glyph is about a
 * millimetre tall; 2 gives an effective 16x16 character, which is readable
 * and still leaves 120 columns by 67 rows. */
#define FBCON_SCALE 2

bool fbcon_init(void);
bool fbcon_active(void);

void fbcon_putc(char c);
void fbcon_clear(void);
void fbcon_home(void);

int  fbcon_cols(void);
int  fbcon_rows(void);

/* Push any pending output to the display. */
void fbcon_flush(void);

#endif /* FBCON_H */
