#include "mouse.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "console.h"

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

static int      pos_x, pos_y;
static int      bound_w = 320, bound_h = 200;
static uint8_t  buttons;
static uint8_t  pending_clicks;
static uint32_t packets;
static bool     present;

/* Three-byte packets arrive one byte per interrupt. */
static uint8_t  packet[3];
static int      packet_index;

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

    if (packet_index < 3)
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
    pending_clicks |= (uint8_t)(now & ~buttons);
    buttons = now;
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

    if (!mouse_write(0xF4)) goto absent;
    if (mouse_read() != 0xFA) goto absent;

    present = true;

    isr_register(IRQ_BASE + IRQ_MOUSE, mouse_isr);

    /* IRQ 12 is on the slave PIC, which reaches the CPU through the master's
     * line 2. Unmasking the slave's line alone is not enough — the cascade
     * line has to be open too, or the interrupt never arrives. */
    pic_unmask(2);
    pic_unmask(IRQ_MOUSE);

    kprintf("mouse            : ps/2, irq 12, reporting enabled\n");
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

void mouse_set_bounds(int w, int h)
{
    bound_w = w;
    bound_h = h;

    if (pos_x >= w) pos_x = w - 1;
    if (pos_y >= h) pos_y = h - 1;
}

uint32_t mouse_packet_count(void) { return packets; }
bool     mouse_present(void)      { return present; }
