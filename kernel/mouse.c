#include "mouse.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "console.h"
#include "task.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_FROM_MOUSE  0x20

#define IRQ_MOUSE 12

/* Packet byte 0 flag bits. */
#define FLAG_ALWAYS_ONE 0x08
#define FLAG_X_SIGN     0x10
#define FLAG_Y_SIGN     0x20
#define FLAG_X_OVERFLOW 0x40
#define FLAG_Y_OVERFLOW 0x80

/* Wheel support changes the packet size, so it has to be known before any
 * packet is decoded. */
static bool     has_wheel;
static int      wheel_delta;

static int      pos_x, pos_y;
static int      bound_w = 320, bound_h = 200;
static uint8_t  buttons;
static uint8_t  pending_clicks;
static uint32_t packets;
static bool     present;

/* Packets arrive one byte per interrupt: three bytes normally, four once the
 * wheel is enabled. */
static uint8_t  packet[4];
static int      packet_index;
static int      packet_size = 3;

/* The controller is slow relative to the CPU; both directions need waiting on.
 * Bounded rather than infinite so a missing or wedged controller cannot hang
 * the kernel during boot. */
static bool wait_writable(void)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(PS2_STATUS) & STATUS_INPUT_FULL) == 0)
            return true;
    return false;
}

static bool wait_readable(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    return false;
}

/* Commands destined for the mouse rather than the keyboard must be prefixed
 * with 0xD4, which tells the controller to forward the next byte to the
 * auxiliary device. */
static bool mouse_write(uint8_t value)
{
    if (!wait_writable()) return false;
    outb(PS2_COMMAND, 0xD4);

    if (!wait_writable()) return false;
    outb(PS2_DATA, value);

    return true;
}

static uint8_t mouse_read(void)
{
    if (!wait_readable())
        return 0xFF;
    return inb(PS2_DATA);
}

static void mouse_isr(registers_t *regs)
{
    (void)regs;

    /* Byte 0 always has bit 3 set. Using that to resynchronise means a dropped
     * or spurious byte costs one packet instead of permanently offsetting the
     * stream, which would leave the cursor moving in the wrong axis for ever. */
    uint8_t value = inb(PS2_DATA);

    if (packet_index == 0 && !(value & FLAG_ALWAYS_ONE))
        return;     /* not a valid first byte; discard and stay in sync */

    packet[packet_index++] = value;

    if (packet_index < packet_size)
        return;

    packet_index = 0;
    packets++;

    uint8_t flags = packet[0];

    /* Overflow means the movement exceeded what the packet can express. The
     * values are meaningless in that case, so the packet is dropped. */
    if (flags & (FLAG_X_OVERFLOW | FLAG_Y_OVERFLOW))
        return;

    /* Movement is a 9-bit signed value: 8 bits of magnitude plus a sign bit
     * living in the flags byte. Sign-extend by hand. */
    int dx = packet[1];
    int dy = packet[2];

    if (flags & FLAG_X_SIGN) dx |= 0xFFFFFF00;
    if (flags & FLAG_Y_SIGN) dy |= 0xFFFFFF00;

    pos_x += dx;
    pos_y -= dy;    /* the mouse reports Y upwards; the screen counts downwards */

    if (pos_x < 0) pos_x = 0;
    if (pos_y < 0) pos_y = 0;
    if (pos_x >= bound_w) pos_x = bound_w - 1;
    if (pos_y >= bound_h) pos_y = bound_h - 1;

    /* Record presses as edges. A GUI wants to know a click happened, not that
     * a button is currently held, and polling for level would fire repeatedly
     * for one press. */
    uint8_t now = flags & 0x07;
    uint8_t old_buttons = buttons;
    pending_clicks |= (uint8_t)(now & ~buttons);
    buttons = now;

    /* The fourth byte is a signed 4-bit wheel movement in its low nibble:
     * negative for scrolling down, positive for up. The upper nibble carries
     * the extra buttons on a five-button mouse, which are ignored here. */
    int8_t z = 0;
    if (has_wheel) {
        z = (int8_t)(packet[3] & 0x0F);
        if (z & 0x08)
            z |= (int8_t)0xF0;      /* sign-extend from four bits */

        wheel_delta += z;
    }

    /* Position is overwritten in place, so a burst of packets while the GUI
     * is halted collapses to the last sample. Wake only on a real change:
     * empty packets still arrive and would otherwise bounce the waiter off
     * hlt at full speed. */
    if (dx != 0 || dy != 0 || now != old_buttons || z != 0)
        task_idle_nudge();
}

void mouse_init(void)
{
    packet_index = 0;
    pos_x = bound_w / 2;
    pos_y = bound_h / 2;

    /* Enable the auxiliary device. */
    if (!wait_writable()) goto absent;
    outb(PS2_COMMAND, 0xA8);

    /* Read the controller configuration byte, set the bit that enables the
     * auxiliary interrupt, and write it back. */
    if (!wait_writable()) goto absent;
    outb(PS2_COMMAND, 0x20);

    uint8_t config = mouse_read();
    config |= 0x02;         /* enable IRQ 12 */
    config &= (uint8_t)~0x20;  /* clear the auxiliary clock disable bit */

    if (!wait_writable()) goto absent;
    outb(PS2_COMMAND, 0x60);
    if (!wait_writable()) goto absent;
    outb(PS2_DATA, config);

    /* Restore defaults, then enable reporting. Each command is acknowledged
     * with 0xFA; anything else means nothing is listening. */
    if (!mouse_write(0xF6)) goto absent;
    if (mouse_read() != 0xFA) goto absent;

    /* Ask for the wheel.
     *
     * A plain PS/2 mouse sends three-byte packets and has no scroll wheel.
     * The extended protocol is not requested by a command -- it is unlocked
     * by setting the sample rate to 200, then 100, then 80 in that exact
     * order, after which the device reports ID 3 instead of 0 and starts
     * sending a fourth byte carrying the wheel movement. It is a knock rather
     * than a question, and a mouse that does not recognise it simply stays in
     * the three-byte protocol, which is why this is safe to attempt blindly. */
    static const uint8_t knock[3] = { 200, 100, 80 };

    for (int i = 0; i < 3; i++) {
        if (!mouse_write(0xF3)) goto absent;    /* set sample rate */
        if (mouse_read() != 0xFA) goto absent;
        if (!mouse_write(knock[i])) goto absent;
        if (mouse_read() != 0xFA) goto absent;
    }

    if (mouse_write(0xF2) && mouse_read() == 0xFA) {
        uint8_t id = mouse_read();
        has_wheel   = (id == 3);
        packet_size = has_wheel ? 4 : 3;
    }

    if (!mouse_write(0xF4)) goto absent;
    if (mouse_read() != 0xFA) goto absent;

    present = true;

    isr_register(IRQ_BASE + IRQ_MOUSE, mouse_isr);

    /* IRQ 12 is on the slave PIC, which reaches the CPU through the master's
     * line 2. Unmasking the slave's line alone is not enough — the cascade
     * line has to be open too, or the interrupt never arrives. */
    pic_unmask(2);
    pic_unmask(IRQ_MOUSE);

    kprintf("mouse            : ps/2, irq 12, %s\n",
            has_wheel ? "wheel enabled, 4-byte packets"
                      : "no wheel, 3-byte packets");
    return;

absent:
    present = false;
    kprintf("mouse            : no ps/2 mouse detected (gui will run without one)\n");
}

int mouse_x(void) { return pos_x; }
int mouse_y(void) { return pos_y; }

uint8_t mouse_buttons(void) { return buttons; }

bool mouse_take_click(uint8_t button)
{
    if (pending_clicks & button) {
        pending_clicks &= (uint8_t)~button;
        return true;
    }
    return false;
}

bool mouse_has_pending_input(void)
{
    return pending_clicks != 0 || wheel_delta != 0;
}

void mouse_set_bounds(int w, int h)
{
    bound_w = w;
    bound_h = h;

    if (pos_x >= w) pos_x = w - 1;
    if (pos_y >= h) pos_y = h - 1;
}

uint32_t mouse_packet_count(void) { return packets; }
bool     mouse_present(void)      { return present; }


int mouse_take_wheel(void)
{
    int d = wheel_delta;
    wheel_delta = 0;
    return d;
}

bool mouse_has_wheel(void) { return has_wheel; }
