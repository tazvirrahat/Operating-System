#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "console.h"

#include <stdint.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* Scancode set 1 sends a "make" code when a key goes down and a "break" code
 * when it comes back up. The break code is the make code with bit 7 set, so a
 * naive handler that treats every event as a press doubles every character. */
#define BREAK_BIT 0x80

/* Extended keys (arrows, right ctrl, ...) are preceded by this prefix byte. */
#define EXTENDED_PREFIX 0xE0

#define BUFFER_SIZE 128

static volatile char     buffer[BUFFER_SIZE];
static volatile unsigned head;      /* written by the IRQ handler */
static volatile unsigned tail;      /* read by consumers */
static volatile unsigned irq_count;

static bool shift_held;
static bool caps_lock;
static bool expecting_extended;

/* US QWERTY layout, indexed by make code. Zero means "no printable character",
 * which covers modifiers, function keys and unassigned codes. */
static const char keymap[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6',    /* 0x00 */
    '7',  '8', '9', '0', '-', '=', '\b', '\t',  /* 0x08 */
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',    /* 0x10 */
    'o',  'p', '[', ']', '\n', 0,  'a', 's',    /* 0x18  0x1D = left ctrl */
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',    /* 0x20 */
    '\'', '`',  0,  '\\','z', 'x', 'c', 'v',    /* 0x28  0x2A = left shift */
    'b',  'n', 'm', ',', '.', '/',  0,  '*',    /* 0x30  0x36 = right shift */
    0,    ' ',  0,   0,   0,   0,   0,   0,     /* 0x38  0x38 = alt, 0x3A = caps */
};

/* Same table with shift applied. */
static const char keymap_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^',
    '&',  '*', '(', ')', '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O',  'P', '{', '}', '\n', 0,  'A', 'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"',  '~',  0,  '|', 'Z', 'X', 'C', 'V',
    'B',  'N', 'M', '<', '>', '?',  0,  '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,
};

static void buffer_push(char c)
{
    unsigned next = (head + 1) % BUFFER_SIZE;

    /* Drop the character rather than overwrite unread input. A full buffer
     * means the consumer is not keeping up, and losing the newest keystroke
     * is less confusing than losing an older one. */
    if (next == tail)
        return;

    buffer[head] = c;
    head = next;
}

static void keyboard_isr(registers_t *regs)
{
    (void)regs;

    irq_count++;

    uint8_t code = inb(KBD_DATA);

    if (code == EXTENDED_PREFIX) {
        expecting_extended = true;
        return;
    }

    if (expecting_extended) {
        /* Arrow keys and friends. Nothing consumes them yet, so swallow the
         * second byte rather than letting it be decoded as an unrelated key. */
        expecting_extended = false;
        return;
    }

    bool released = (code & BREAK_BIT) != 0;
    uint8_t make  = code & ~BREAK_BIT;

    /* Modifiers are state, not characters: both press and release matter. */
    if (make == 0x2A || make == 0x36) {     /* left / right shift */
        shift_held = !released;
        return;
    }

    if (released)
        return;                             /* every other key: presses only */

    if (make == 0x3A) {                     /* caps lock toggles on press */
        caps_lock = !caps_lock;
        return;
    }

    if (make >= 128)
        return;

    char c = shift_held ? keymap_shift[make] : keymap[make];

    /* Caps lock affects letters only, unlike shift. */
    if (caps_lock && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (caps_lock && shift_held && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    if (c)
        buffer_push(c);
}

void kbd_init(void)
{
    head = tail = 0;
    shift_held = caps_lock = expecting_extended = false;

    /* Drain anything the BIOS left in the controller, or the first real
     * keystroke will be preceded by stale bytes. */
    while (inb(KBD_STATUS) & 0x01)
        (void)inb(KBD_DATA);

    isr_register(IRQ_BASE + IRQ_KEYBOARD, keyboard_isr);
    pic_unmask(IRQ_KEYBOARD);

    kprintf("keyboard         : ps/2, irq 1 unmasked, %u byte buffer\n",
            BUFFER_SIZE);
}

bool kbd_available(void)
{
    return head != tail;
}

char kbd_poll(void)
{
    if (head == tail)
        return 0;

    char c = buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return c;
}

char kbd_getchar(void)
{
    for (;;) {
        char c = kbd_poll();
        if (c)
            return c;

        /* Sleep until any interrupt arrives rather than spinning. The timer
         * alone wakes us 100 times a second, so this stays responsive. */
        __asm__ volatile ("hlt");
    }
}

unsigned kbd_irq_count(void)
{
    return irq_count;
}
