/* wallpaper.h — desktop backgrounds.
 *
 * There is no filesystem, so a wallpaper cannot be a file. These are drawn
 * procedurally instead: gradients and simple shapes computed per band, which
 * costs a handful of fills and needs no storage at all. That is also why they
 * scale to whatever resolution the firmware gave us without any resampling.
 */
#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdint.h>

#define WALLPAPER_COUNT 6

/* Draw the current wallpaper into the region above the taskbar. */
void wallpaper_draw(int w, int h);

/* Draw only the part of the wallpaper inside the clip rectangle.
 *
 * The desktop is repainted far more often than it changes. When a window
 * moves, the only wallpaper that needs redrawing is the strip it uncovered,
 * and at 1920x1080 that is the difference between a few hundred kilobytes and
 * the whole screen. */
void wallpaper_draw_clip(int w, int h, int cx, int cy, int cw, int ch);

void        wallpaper_next(void);
void        wallpaper_set(int index);
int         wallpaper_current(void);
const char *wallpaper_name(int index);

#endif /* WALLPAPER_H */
