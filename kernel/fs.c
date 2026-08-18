#include "fs.h"
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "console.h"

static fs_file_t files[FS_MAX_FILES];

void fs_init(void)
{
    memset(files, 0, sizeof(files));

    /* A couple of files so the filesystem is not empty on a fresh boot and
     * `ls` shows something on first use. */
    static const char readme[] =
        "MyOS in-memory filesystem.\n"
        "\n"
        "Files live in the kernel heap, so nothing here survives a reboot.\n"
        "There is no disk driver: reaching real storage would need an ATA\n"
        "or AHCI driver and an on-disk format on top of it.\n"
        "\n"
        "Try: ls, cat readme.txt, write notes.txt hello, rm notes.txt\n";

    fs_write("readme.txt", readme, (uint32_t)(sizeof(readme) - 1));

    static const char about[] =
        "MyOS - a bare metal x86 kernel.\n"
        "Preemptive scheduling, paging, ring 3, PCI, a GUI.\n";

    fs_write("about.txt", about, (uint32_t)(sizeof(about) - 1));

    kprintf("filesystem       : in-memory, %d slots, %d files present\n",
            FS_MAX_FILES, fs_file_count());
}

static void stamp(fs_file_t *f)
{
    rtc_time_t now;
    rtc_read(&now);

    f->hour   = now.hour;
    f->minute = now.minute;
    f->day    = now.day;
    f->month  = now.month;
    f->year   = now.year;
}

fs_file_t *fs_find(const char *name)
{
    if (!name || !*name)
        return 0;

    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return &files[i];

    return 0;
}

fs_file_t *fs_create(const char *name)
{
    if (!name || !*name || strlen(name) >= FS_NAME_MAX)
        return 0;

    fs_file_t *existing = fs_find(name);
    if (existing)
        return existing;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used)
            continue;

        memset(&files[i], 0, sizeof(files[i]));
        strncpy(files[i].name, name, FS_NAME_MAX - 1);
        files[i].used = true;
        stamp(&files[i]);

        return &files[i];
    }

    return 0;   /* table full */
}

/* Make sure a file can hold `needed` bytes, reallocating if not.
 *
 * There is no realloc in this kernel, so growth is allocate-copy-free by
 * hand. Capacity is grown in steps rather than to the exact size, so
 * repeatedly appending a few bytes does not reallocate on every call. */
static bool ensure_capacity(fs_file_t *f, uint32_t needed)
{
    if (needed > FS_MAX_SIZE)
        return false;

    if (f->capacity >= needed)
        return true;

    uint32_t want = f->capacity ? f->capacity : 256;
    while (want < needed)
        want *= 2;

    if (want > FS_MAX_SIZE)
        want = FS_MAX_SIZE;

    uint8_t *block = kmalloc(want);
    if (!block)
        return false;

    if (f->data) {
        memcpy(block, f->data, f->size);
        kfree(f->data);
    }

    f->data     = block;
    f->capacity = want;
    return true;
}

bool fs_write(const char *name, const void *data, uint32_t size)
{
    fs_file_t *f = fs_create(name);
    if (!f || !ensure_capacity(f, size))
        return false;

    if (size && data)
        memcpy(f->data, data, size);

    f->size = size;
    stamp(f);
    return true;
}

bool fs_append(const char *name, const void *data, uint32_t size)
{
    fs_file_t *f = fs_create(name);
    if (!f || !ensure_capacity(f, f->size + size))
        return false;

    if (size && data)
        memcpy(f->data + f->size, data, size);

    f->size += size;
    stamp(f);
    return true;
}

bool fs_delete(const char *name)
{
    fs_file_t *f = fs_find(name);
    if (!f)
        return false;

    if (f->data)
        kfree(f->data);

    memset(f, 0, sizeof(*f));
    return true;
}

const fs_file_t *fs_at(int index)
{
    if (index < 0 || index >= FS_MAX_FILES || !files[index].used)
        return 0;

    return &files[index];
}

int fs_file_count(void)
{
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used)
            n++;
    return n;
}

uint32_t fs_bytes_used(void)
{
    uint32_t total = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used)
            total += files[i].size;
    return total;
}
