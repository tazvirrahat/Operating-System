#include "svga.h"
#include "pci.h"
#include "io.h"
#include "console.h"
#include "paging.h"

/* Register indices. The adapter exposes two ports: write the index of the
 * register you want to the first, then read or write its value through the
 * second. */
#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_FB_SIZE        16
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_FIFO_START     18
#define SVGA_REG_FIFO_SIZE      19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22

/* Version handshake. The driver writes the highest version it understands and
 * reads it back; the adapter lowers the value if it speaks something older. */
/* Unsigned throughout: the magic shifted left by 8 is 0x90000000, which does
 * not fit in a signed int, and the registers these are compared against are
 * unsigned. */
#define SVGA_MAGIC      0x900000u
#define SVGA_ID_2       ((SVGA_MAGIC << 8) | 2u)
#define SVGA_ID_1       ((SVGA_MAGIC << 8) | 1u)
#define SVGA_ID_0       ((SVGA_MAGIC << 8) | 0u)

/* Capability bits.
 *
 * These are the values from the original SVGA register specification, which
 * is the one the accelerated rectangle commands belong to. Later revisions of
 * the header renumbered the upper bits for 3D and memory-region features, and
 * taking the values from there put fill and copy at 0x10 and 0x20 — so an
 * adapter reporting 0x03, meaning it supports both, was read as supporting
 * neither, and every fill fell back to software for no reason. */
#define SVGA_CAP_RECT_FILL 0x00000001
#define SVGA_CAP_RECT_COPY 0x00000002

/* The first four words of FIFO memory are a ring buffer header rather than
 * commands: where the ring starts and ends, where the next command goes, and
 * how far the adapter has consumed. */
#define SVGA_FIFO_MIN      0
#define SVGA_FIFO_MAX      1
#define SVGA_FIFO_NEXT_CMD 2
#define SVGA_FIFO_STOP     3

#define SVGA_CMD_UPDATE    1
#define SVGA_CMD_RECT_FILL 2
#define SVGA_CMD_RECT_COPY 3

static uint16_t  io_base;
static uint32_t *fifo;
static uint32_t  fifo_size;
static uint32_t  fb_address;
static uint32_t  capabilities;
static uint32_t  commands_issued;
static bool      present;

static void reg_write(uint32_t index, uint32_t value)
{
    outl((uint16_t)(io_base + SVGA_INDEX_PORT), index);
    outl((uint16_t)(io_base + SVGA_VALUE_PORT), value);
}

static uint32_t reg_read(uint32_t index)
{
    outl((uint16_t)(io_base + SVGA_INDEX_PORT), index);
    return inl((uint16_t)(io_base + SVGA_VALUE_PORT));
}

bool svga_init(void)
{
    const pci_device_t *dev = pci_find(SVGA_VENDOR_ID, SVGA_DEVICE_ID);

    if (!dev) {
        present = false;
        return false;
    }

    /* BAR0 holds the I/O port base. The address it reports is where the
     * firmware decided to put this device on this machine — the reason PCI
     * enumeration had to come first. */
    io_base = (uint16_t)pci_bar_address(dev->bar[0]);

    if (io_base == 0) {
        present = false;
        return false;
    }

    /* Negotiate the protocol version, newest first. */
    reg_write(SVGA_REG_ID, SVGA_ID_2);
    if (reg_read(SVGA_REG_ID) != SVGA_ID_2) {
        reg_write(SVGA_REG_ID, SVGA_ID_1);
        if (reg_read(SVGA_REG_ID) != SVGA_ID_1) {
            reg_write(SVGA_REG_ID, SVGA_ID_0);
            if (reg_read(SVGA_REG_ID) != SVGA_ID_0) {
                present = false;
                return false;
            }
        }
    }

    fb_address   = reg_read(SVGA_REG_FB_START);
    capabilities = reg_read(SVGA_REG_CAPABILITIES);

    uint32_t fifo_address = reg_read(SVGA_REG_FIFO_START);
    fifo_size             = reg_read(SVGA_REG_FIFO_SIZE);

    if (!fifo_address || fifo_size < 16) {
        present = false;
        return false;
    }

    /* The FIFO is device memory at an address the firmware chose, so it is
     * outside anything mapped at startup. Touching it before mapping it would
     * page fault on the first word written. */
    paging_map_region(fifo_address, fifo_size);
    paging_map_region(fb_address, reg_read(SVGA_REG_FB_SIZE));

    fifo = (uint32_t *)fifo_address;

    /* Set up the command ring. Commands start after the four header words. */
    fifo[SVGA_FIFO_MIN]      = 16;
    fifo[SVGA_FIFO_MAX]      = fifo_size;
    fifo[SVGA_FIFO_NEXT_CMD] = 16;
    fifo[SVGA_FIFO_STOP]     = 16;

    reg_write(SVGA_REG_CONFIG_DONE, 1);

    present = true;

    kprintf("svga             : vmware svga-ii at i/o %04x, fb %08x, fifo %u KB\n",
            io_base, fb_address, fifo_size / 1024);
    kprintf("                 : accelerated fill %s, copy %s\n",
            svga_can_fill() ? "yes" : "no",
            svga_can_copy() ? "yes" : "no");

    return true;
}

bool svga_available(void) { return present; }
bool svga_can_fill(void)  { return present && (capabilities & SVGA_CAP_RECT_FILL); }
bool svga_can_copy(void)  { return present && (capabilities & SVGA_CAP_RECT_COPY); }

uint32_t svga_framebuffer(void)       { return fb_address; }
uint32_t svga_fifo_capabilities(void) { return capabilities; }
uint32_t svga_command_count(void)     { return commands_issued; }

const char *svga_name(void) { return present ? "VMware SVGA-II" : "none"; }

/* Push one word into the command ring.
 *
 * The ring wraps, and the write position must never catch up with the point
 * the adapter has read to, or commands already queued would be overwritten
 * before being executed. When that is about to happen the only safe option is
 * to wait for the adapter to drain. */
static void fifo_write(uint32_t value)
{
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t stop = fifo[SVGA_FIFO_STOP];
    uint32_t min  = fifo[SVGA_FIFO_MIN];
    uint32_t max  = fifo[SVGA_FIFO_MAX];

    uint32_t next_after = next + 4;
    if (next_after >= max)
        next_after = min;

    if (next_after == stop)
        svga_sync();

    fifo[next / 4] = value;
    fifo[SVGA_FIFO_NEXT_CMD] = next_after;
}

void svga_sync(void)
{
    if (!present)
        return;

    reg_write(SVGA_REG_SYNC, 1);

    /* Bounded rather than infinite: a wedged adapter should not hang the
     * kernel, and dropping a frame is survivable. */
    for (int i = 0; i < 1000000; i++)
        if (!reg_read(SVGA_REG_BUSY))
            return;
}

void svga_update(int x, int y, int w, int h)
{
    if (!present || w <= 0 || h <= 0)
        return;

    fifo_write(SVGA_CMD_UPDATE);
    fifo_write((uint32_t)x);
    fifo_write((uint32_t)y);
    fifo_write((uint32_t)w);
    fifo_write((uint32_t)h);

    commands_issued++;
}

bool svga_fill_rect(int x, int y, int w, int h, uint32_t colour)
{
    if (!svga_can_fill() || w <= 0 || h <= 0)
        return false;

    fifo_write(SVGA_CMD_RECT_FILL);
    fifo_write(colour);
    fifo_write((uint32_t)x);
    fifo_write((uint32_t)y);
    fifo_write((uint32_t)w);
    fifo_write((uint32_t)h);

    commands_issued++;
    return true;
}

bool svga_copy_rect(int sx, int sy, int dx, int dy, int w, int h)
{
    if (!svga_can_copy() || w <= 0 || h <= 0)
        return false;

    fifo_write(SVGA_CMD_RECT_COPY);
    fifo_write((uint32_t)sx);
    fifo_write((uint32_t)sy);
    fifo_write((uint32_t)dx);
    fifo_write((uint32_t)dy);
    fifo_write((uint32_t)w);
    fifo_write((uint32_t)h);

    commands_issued++;
    return true;
}
