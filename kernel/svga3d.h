/* svga3d.h — the SVGA3D command set on the VMware adapter.
 *
 * The 2D driver in svga.c pushes rectangle commands into a queue. This is the
 * layer above that: it drives the adapter's actual 3D pipeline, where the
 * guest defines surfaces (images living in host video memory), creates a
 * rendering context, binds a surface as the render target, issues drawing
 * commands against it, and finally presents it to the screen.
 *
 * Availability is not assumed. The adapter must advertise SVGA_CAP_3D, and
 * separately report a non-zero 3D hardware version through the extended FIFO
 * register block -- a device can have the capability bit and still refuse to
 * expose a pipeline. Both are checked before anything is submitted, because a
 * command the device does not understand is discarded silently rather than
 * reported, which makes the failure invisible.
 */
#ifndef SVGA3D_H
#define SVGA3D_H

#include <stdint.h>
#include <stdbool.h>

bool     svga3d_init(void);
bool     svga3d_available(void);

/* Hardware version the adapter reports. Zero means no pipeline. */
uint32_t svga3d_hwversion(void);

/* Number of 3D commands submitted, for reporting. */
uint32_t svga3d_command_count(void);

/* Clear the render target to a colour, then present it to the screen.
 * Returns false if 3D is unavailable. */
bool svga3d_clear(uint32_t argb);
bool svga3d_present(void);

/* Fill a rectangle by clearing a scissored region of the render target.
 * The clear command takes a rectangle list, so this is a genuine
 * hardware-accelerated fill even though the adapter dropped RECT_FILL. */
bool svga3d_fill_rect(int x, int y, int w, int h, uint32_t argb);

#endif /* SVGA3D_H */
