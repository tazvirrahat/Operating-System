/* wallpaper_image.h — the embedded desktop picture.
 *
 * Defined either by kernel/wallpaper_image.c, which tools/genwallpaper.py
 * generates from a real image, or by the fallback in wallpaper.c when no
 * image has been generated. The kernel never parses an image format: what
 * arrives here is already a palette and one index per pixel.
 */
#ifndef WALLPAPER_IMAGE_H
#define WALLPAPER_IMAGE_H

#include <stdint.h>

/* Zero when no picture is embedded, which is how wallpaper.c knows to fall
 * back to a generated gradient rather than reading an empty array. */
extern const int wallpaper_image_w;
extern const int wallpaper_image_h;

extern const uint8_t wallpaper_image_palette[256 * 3];
extern const uint8_t wallpaper_image_pixels[];

#endif /* WALLPAPER_IMAGE_H */
