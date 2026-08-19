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

void        wallpaper_next(void);
void        wallpaper_set(int index);
int         wallpaper_current(void);
const char *wallpaper_name(int index);

#endif /* WALLPAPER_H */
