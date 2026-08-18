/* svga.h — VMware SVGA-II graphics adapter driver.
 *
 * This is a real device driver for a real graphics card, found on the PCI bus
 * rather than assumed to exist at a fixed address, and driven through a
 * command FIFO rather than by writing pixels one at a time.
 *
 * What it buys over the plain framebuffer: rectangle fills and copies are
 * handed to the adapter as commands instead of being executed by the CPU.
 * Clearing a 1024x768 desktop in software means writing 786,432 pixels; as an
 * accelerated fill it is six words pushed into a queue.
 *
 * The adapter is optional. When it is absent — under QEMU's default VGA, or
 * on real hardware — the framebuffer path is used unchanged. Detection is by
 * PCI ID, so nothing here runs on a machine that does not have the device.
 */
#ifndef SVGA_H
#define SVGA_H

#include <stdint.h>
#include <stdbool.h>

#define SVGA_VENDOR_ID 0x15AD
#define SVGA_DEVICE_ID 0x0405

bool svga_init(void);
bool svga_available(void);

/* True when the adapter advertises the corresponding accelerated operation.
 * Capabilities are read from the device, not assumed: not every revision
 * implements every command. */
bool svga_can_fill(void);
bool svga_can_copy(void);

uint32_t svga_framebuffer(void);
uint32_t svga_fifo_capabilities(void);

/* Accelerated operations. Each returns false if the adapter cannot do it, so
 * the caller can fall back to software. */
bool svga_fill_rect(int x, int y, int w, int h, uint32_t colour);
bool svga_copy_rect(int sx, int sy, int dx, int dy, int w, int h);

/* Tell the adapter a region of the framebuffer has changed and should be
 * shown. Writing to the framebuffer alone does not update the display. */
void svga_update(int x, int y, int w, int h);

/* Block until the adapter has finished the queued commands. */
void svga_sync(void);

/* Reporting. */
uint32_t svga_command_count(void);
const char *svga_name(void);


/* Raw capability word and 3D bits, for reporting. */
uint32_t svga_raw_caps(void);
bool     svga_has_3d(void);
uint32_t svga_fifo_min(void);


/* ---- used by the 3D layer ---- */

/* Push a raw word into the command ring. The 3D layer builds its own command
 * headers and needs the ring directly. */
void     svga_fifo_raw(uint32_t value);

/* 3D hardware version from the extended register block. Zero means no
 * pipeline, even when the 3D capability bit is set. */
uint32_t svga_fifo_3d_hwversion(void);
uint32_t svga_fifo_caps(void);

uint32_t svga_screen_width(void);
uint32_t svga_screen_height(void);

#endif /* SVGA_H */
