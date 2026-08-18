/* mouse.h — PS/2 mouse driver.
 *
 * The mouse hangs off the same 8042 controller as the keyboard, on the
 * auxiliary port, and interrupts on IRQ 12. That matters: an earlier version
 * of this project targeted ARM, where keyboard and mouse arrive over USB and
 * a driver means implementing a USB host stack of some ten thousand lines.
 * On x86 the legacy PS/2 path is emulated by every virtual machine and by
 * real hardware through the BIOS, and the whole driver is about a hundred
 * lines. That difference is the only reason a mouse is feasible here.
 */
#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

void mouse_init(void);

/* Current position, clamped to the screen bounds set by mouse_set_bounds. */
int  mouse_x(void);
int  mouse_y(void);

/* Bitmask of MOUSE_LEFT / MOUSE_RIGHT / MOUSE_MIDDLE. */
uint8_t mouse_buttons(void);

/* True if a button went down since the last call. Consumed by reading. */
bool mouse_take_click(uint8_t button);

void mouse_set_bounds(int w, int h);

/* Packets received since boot, for diagnostics. */
uint32_t mouse_packet_count(void);

/* False if no PS/2 mouse answered during initialisation. */
bool mouse_present(void);

/* Accumulated wheel movement since the last call, positive for scrolling up.
 * Reading it consumes it. */
int  mouse_take_wheel(void);
bool mouse_has_wheel(void);

#endif /* MOUSE_H */
