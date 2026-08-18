#include "svga3d.h"
#include "svga.h"
#include "console.h"

/* Command IDs. The 3D command space starts at 1040, above the 2D commands. */
#define SVGA_3D_CMD_SURFACE_DEFINE   1040
#define SVGA_3D_CMD_SURFACE_DESTROY  1041
#define SVGA_3D_CMD_SURFACE_DMA      1044
#define SVGA_3D_CMD_CONTEXT_DEFINE   1045
#define SVGA_3D_CMD_CONTEXT_DESTROY  1046
#define SVGA_3D_CMD_SETRENDERTARGET  1050
#define SVGA_3D_CMD_SETVIEWPORT      1055
#define SVGA_3D_CMD_CLEAR            1057
#define SVGA_3D_CMD_PRESENT          1058

#define SVGA3D_X8R8G8B8              1
#define SVGA3D_A8R8G8B8              2

#define SVGA3D_SURFACE_HINT_RENDERTARGET (1 << 6)

#define SVGA3D_CLEAR_COLOR           0x1
#define SVGA3D_RT_COLOR0             2
#define SVGA3D_MAX_SURFACE_FACES     6

/* Identifiers we allocate for ourselves. The guest picks these; the device
 * only requires that they are unique and within its limits. */
#define OUR_CONTEXT_ID 1
#define OUR_SURFACE_ID 1

static bool     ready;
static uint32_t hwversion;
static uint32_t commands;
static uint32_t rt_width, rt_height;

/* ---- command submission --------------------------------------------------
 *
 * Every 3D command is a header of {id, size} followed by a payload, where
 * size counts the payload bytes only. Getting that count wrong is the classic
 * way to lose an afternoon: the device reads the stated number of bytes and
 * then treats whatever follows as the next command header, so one wrong size
 * desynchronises the entire queue and everything after it is garbage. The
 * helpers below compute it from what was actually written rather than from a
 * number typed by hand.
 */

static uint32_t payload_words;

static void cmd_begin(uint32_t id)
{
    (void)id;
    payload_words = 0;
}

/* Words are buffered so the header can carry an accurate size. A 3D command
 * is at most a few dozen words, so a small fixed buffer is enough. */
#define MAX_CMD_WORDS 64
static uint32_t cmd_buffer[MAX_CMD_WORDS];

static void cmd_word(uint32_t value)
{
    if (payload_words < MAX_CMD_WORDS)
        cmd_buffer[payload_words++] = value;
}

static void cmd_submit(uint32_t id)
{
    svga_fifo_raw(id);
    svga_fifo_raw(payload_words * 4);

    for (uint32_t i = 0; i < payload_words; i++)
        svga_fifo_raw(cmd_buffer[i]);

    commands++;
}

/* ---- setup --------------------------------------------------------------- */

static void define_surface(uint32_t sid, uint32_t width, uint32_t height)
{
    cmd_begin(SVGA_3D_CMD_SURFACE_DEFINE);

    cmd_word(sid);
    cmd_word(SVGA3D_SURFACE_HINT_RENDERTARGET);
    cmd_word(SVGA3D_X8R8G8B8);

    /* One entry per cube face; only the first is used for a flat 2D surface,
     * and the rest must still be present because the structure is fixed. */
    for (int i = 0; i < SVGA3D_MAX_SURFACE_FACES; i++)
        cmd_word(i == 0 ? 1 : 0);   /* numMipLevels */

    /* One mip level: width, height, depth. */
    cmd_word(width);
    cmd_word(height);
    cmd_word(1);

    cmd_submit(SVGA_3D_CMD_SURFACE_DEFINE);
}

static void define_context(uint32_t cid)
{
    cmd_begin(SVGA_3D_CMD_CONTEXT_DEFINE);
    cmd_word(cid);
    cmd_submit(SVGA_3D_CMD_CONTEXT_DEFINE);
}

static void set_render_target(uint32_t cid, uint32_t sid)
{
    cmd_begin(SVGA_3D_CMD_SETRENDERTARGET);

    cmd_word(cid);
    cmd_word(SVGA3D_RT_COLOR0);

    /* SVGA3dSurfaceImageId: surface, face, mipmap. */
    cmd_word(sid);
    cmd_word(0);
    cmd_word(0);

    cmd_submit(SVGA_3D_CMD_SETRENDERTARGET);
}

static void set_viewport(uint32_t cid, uint32_t w, uint32_t h)
{
    cmd_begin(SVGA_3D_CMD_SETVIEWPORT);

    cmd_word(cid);
    cmd_word(0);
    cmd_word(0);
    cmd_word(w);
    cmd_word(h);

    cmd_submit(SVGA_3D_CMD_SETVIEWPORT);
}

bool svga3d_init(void)
{
    ready = false;

    if (!svga_available() || !svga_has_3d())
        return false;

    /* The capability bit says the device understands the command set. The
     * hardware version says a pipeline is actually attached. Both are needed:
     * a device can advertise the capability and still report version zero,
     * in which case every command submitted would be silently discarded. */
    hwversion = svga_fifo_3d_hwversion();

    kprintf("svga3d           : hwversion %u, fifo caps %08x, screen %ux%u\n",
            hwversion, svga_fifo_caps(),
            svga_screen_width(), svga_screen_height());

    if (hwversion == 0) {
        kprintf("svga3d           : no pipeline (hardware version reads zero)\n");
        return false;
    }

    rt_width  = svga_screen_width();
    rt_height = svga_screen_height();

    if (rt_width == 0 || rt_height == 0)
        return false;

    define_surface(OUR_SURFACE_ID, rt_width, rt_height);
    define_context(OUR_CONTEXT_ID);
    set_render_target(OUR_CONTEXT_ID, OUR_SURFACE_ID);
    set_viewport(OUR_CONTEXT_ID, rt_width, rt_height);

    svga_sync();

    ready = true;

    kprintf("svga3d           : pipeline up, hw version %u, %ux%u render target\n",
            hwversion, rt_width, rt_height);

    return true;
}

bool     svga3d_available(void)     { return ready; }
uint32_t svga3d_hwversion(void)     { return hwversion; }
uint32_t svga3d_command_count(void) { return commands; }

/* ---- drawing ------------------------------------------------------------- */

bool svga3d_fill_rect(int x, int y, int w, int h, uint32_t argb)
{
    if (!ready || w <= 0 || h <= 0)
        return false;

    cmd_begin(SVGA_3D_CMD_CLEAR);

    cmd_word(OUR_CONTEXT_ID);
    cmd_word(SVGA3D_CLEAR_COLOR);
    cmd_word(argb);
    cmd_word(0);            /* depth, as a float bit pattern; unused here */
    cmd_word(0);            /* stencil */

    /* The clear takes a list of rectangles, which is what makes it usable as
     * a fill: restricting it to one rectangle clears only that region. The
     * adapter dropped the dedicated RECT_FILL command, so this is how an
     * accelerated fill is expressed on hardware that still has 3D. */
    cmd_word((uint32_t)x);
    cmd_word((uint32_t)y);
    cmd_word((uint32_t)w);
    cmd_word((uint32_t)h);

    cmd_submit(SVGA_3D_CMD_CLEAR);
    return true;
}

bool svga3d_clear(uint32_t argb)
{
    return svga3d_fill_rect(0, 0, (int)rt_width, (int)rt_height, argb);
}

bool svga3d_present(void)
{
    if (!ready)
        return false;

    cmd_begin(SVGA_3D_CMD_PRESENT);

    cmd_word(OUR_SURFACE_ID);

    /* SVGA3dCopyRect: destination x, y, w, h then source x, y. */
    cmd_word(0);
    cmd_word(0);
    cmd_word(rt_width);
    cmd_word(rt_height);
    cmd_word(0);
    cmd_word(0);

    cmd_submit(SVGA_3D_CMD_PRESENT);
    svga_sync();

    return true;
}
