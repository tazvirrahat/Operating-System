#include "fs.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "console.h"

/* On-disk layout (fixed, not a general filesystem):
 *
 *   LBA 0          superblock (magic, version, file count)
 *   LBA 1..8       64 directory entries, 64 bytes each, 8 per sector
 *   LBA 16..8207   64 file slots of 128 sectors (64 KB) each
 *
 * Slot i always lives at LBA 16 + i*128, used or not. Write-through then
 * means "rewrite the table plus that one slot", not a free-space search.
 * 64 * 64 KB is 4 MB of file data; an 8 MB image leaves the rest unused
 * (the ATA self-test borrows the last sector as scratch).
 *
 * Magic valid but counts/sizes that cannot be true → format rather than
 * crash. No disk, or a disk smaller than this layout → RAM only, with a
 * boot line that says so. */

#define TAZFS_MAGIC          0x315A4154u  /* 'TAZ1' little-endian */
#define TAZFS_VERSION        1
#define TAZFS_TABLE_LBA      1
#define TAZFS_TABLE_SECTORS  8
#define TAZFS_DATA_LBA       16
#define TAZFS_SLOT_SECTORS   (FS_MAX_SIZE / ATA_SECTOR_SIZE)
#define TAZFS_ENTRY_SIZE     64
#define TAZFS_MIN_SECTORS    (TAZFS_DATA_LBA + FS_MAX_FILES * TAZFS_SLOT_SECTORS)

static fs_file_t files[FS_MAX_FILES];
static bool      persist_enabled;
static int       persist_defer;
static bool      on_disk;
static const char *boot_state = "volatile (no disk)";
static const char *last_error = "";

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint32_t slot_lba(int i)
{
    return TAZFS_DATA_LBA + (uint32_t)i * TAZFS_SLOT_SECTORS;
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

static void pack_entry(uint8_t *dst, int i)
{
    const fs_file_t *f = &files[i];

    memset(dst, 0, TAZFS_ENTRY_SIZE);
    memcpy(dst, f->name, FS_NAME_MAX);
    put_u32(dst + 32, f->size);
    put_u32(dst + 36, slot_lba(i));
    dst[40] = f->hour;
    dst[41] = f->minute;
    dst[42] = f->day;
    dst[43] = f->month;
    put_u16(dst + 44, f->year);
    dst[46] = f->used ? 1 : 0;
}

static bool unpack_entry(const uint8_t *src, int i)
{
    fs_file_t *f = &files[i];
    uint32_t size, lba;
    uint8_t used;

    memset(f, 0, sizeof(*f));

    used = src[46];
    if (!used)
        return true;

    size = get_u32(src + 32);
    lba  = get_u32(src + 36);

    if (src[0] == 0 || size > FS_MAX_SIZE || lba != slot_lba(i))
        return false;

    memcpy(f->name, src, FS_NAME_MAX);
    f->name[FS_NAME_MAX - 1] = '\0';
    f->size   = size;
    f->hour   = src[40];
    f->minute = src[41];
    f->day    = src[42];
    f->month  = src[43];
    f->year   = get_u16(src + 44);
    f->used   = true;
    return true;
}

static bool write_super(void)
{
    uint8_t sec[ATA_SECTOR_SIZE];

    memset(sec, 0, sizeof(sec));
    put_u32(sec + 0, TAZFS_MAGIC);
    put_u32(sec + 4, TAZFS_VERSION);
    put_u32(sec + 8, (uint32_t)fs_file_count());
    put_u32(sec + 12, TAZFS_DATA_LBA);
    put_u32(sec + 16, TAZFS_SLOT_SECTORS);
    put_u32(sec + 20, TAZFS_TABLE_SECTORS);
    return ata_write(0, sec);
}

static bool write_table(void)
{
    uint8_t sec[ATA_SECTOR_SIZE];

    for (int s = 0; s < TAZFS_TABLE_SECTORS; s++) {
        memset(sec, 0, sizeof(sec));
        for (int e = 0; e < 8; e++)
            pack_entry(sec + e * TAZFS_ENTRY_SIZE, s * 8 + e);
        if (!ata_write(TAZFS_TABLE_LBA + (uint32_t)s, sec))
            return false;
    }
    return true;
}

static bool write_file_data(int i)
{
    uint8_t sec[ATA_SECTOR_SIZE];
    const fs_file_t *f = &files[i];
    uint32_t nsec, s, off, n;

    if (!f->used || f->size == 0)
        return true;

    nsec = (f->size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    if (nsec > TAZFS_SLOT_SECTORS)
        return false;

    for (s = 0; s < nsec; s++) {
        memset(sec, 0, sizeof(sec));
        off = s * ATA_SECTOR_SIZE;
        n = f->size - off;
        if (n > ATA_SECTOR_SIZE)
            n = ATA_SECTOR_SIZE;
        if (f->data)
            memcpy(sec, f->data + off, n);
        if (!ata_write(slot_lba(i) + s, sec))
            return false;
    }
    return true;
}

/* Superblock + table always; file data only for the slot that changed.
 * The table is 4 KB. Rewriting it on every save is simpler than a journal
 * and still cheap at this size. */
static bool flush_slot(int i)
{
    if (!persist_enabled || persist_defer)
        return true;
    if (!write_super() || !write_table())
        return false;
    if (i >= 0 && !write_file_data(i))
        return false;
    return true;
}

static bool flush_all(void)
{
    int i;

    if (!persist_enabled || persist_defer)
        return true;
    if (!write_super() || !write_table())
        return false;
    for (i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && !write_file_data(i))
            return false;
    return true;
}

static void free_all_data(void)
{
    int i;

    for (i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].data)
            kfree(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
}

/* Split out of ensure_capacity so load can allocate without going through
 * the write path (which would stamp a new time and flush). */
static bool grow_to(fs_file_t *f, uint32_t needed)
{
    uint32_t want;
    uint8_t *block;

    if (needed > FS_MAX_SIZE)
        return false;
    if (f->capacity >= needed)
        return true;

    want = f->capacity ? f->capacity : 256;
    while (want < needed)
        want *= 2;
    if (want > FS_MAX_SIZE)
        want = FS_MAX_SIZE;

    block = kmalloc(want);
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

static bool load_file_data(int i)
{
    uint8_t sec[ATA_SECTOR_SIZE];
    fs_file_t *f = &files[i];
    uint32_t nsec, s, off, n;

    if (f->size == 0)
        return true;

    if (!grow_to(f, f->size))
        return false;

    nsec = (f->size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    for (s = 0; s < nsec; s++) {
        if (!ata_read(slot_lba(i) + s, sec))
            return false;
        off = s * ATA_SECTOR_SIZE;
        n = f->size - off;
        if (n > ATA_SECTOR_SIZE)
            n = ATA_SECTOR_SIZE;
        memcpy(f->data + off, sec, n);
    }
    return true;
}

static bool read_super(uint32_t *file_count)
{
    uint8_t sec[ATA_SECTOR_SIZE];

    if (!ata_read(0, sec))
        return false;
    if (get_u32(sec + 0) != TAZFS_MAGIC)
        return false;
    if (get_u32(sec + 4) != TAZFS_VERSION)
        return false;
    if (get_u32(sec + 12) != TAZFS_DATA_LBA)
        return false;
    if (get_u32(sec + 16) != TAZFS_SLOT_SECTORS)
        return false;
    if (get_u32(sec + 20) != TAZFS_TABLE_SECTORS)
        return false;

    *file_count = get_u32(sec + 8);
    if (*file_count > FS_MAX_FILES)
        return false;
    return true;
}

static bool load_from_disk(void)
{
    uint8_t sec[ATA_SECTOR_SIZE];
    uint32_t declared;
    int used = 0;
    int s, e, i;

    if (!read_super(&declared))
        return false;

    for (s = 0; s < TAZFS_TABLE_SECTORS; s++) {
        if (!ata_read(TAZFS_TABLE_LBA + (uint32_t)s, sec))
            return false;
        for (e = 0; e < 8; e++) {
            i = s * 8 + e;
            if (!unpack_entry(sec + e * TAZFS_ENTRY_SIZE, i))
                return false;
            if (files[i].used)
                used++;
        }
    }

    if ((uint32_t)used != declared)
        return false;

    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used)
            continue;
        if (!load_file_data(i))
            return false;
    }
    return true;
}

static void seed_defaults(bool persistent)
{
    static const char readme_disk[] =
        "TazOS filesystem.\n"
        "\n"
        "Files are stored on the ATA disk and survive a reboot. The same\n"
        "namespace is what `ls`, File Explorer and Notepad all use.\n"
        "\n"
        "Try: ls, cat readme.txt, write notes.txt hello, rm notes.txt\n";

    static const char readme_ram[] =
        "TazOS filesystem (volatile).\n"
        "\n"
        "No disk is attached, so files live in the kernel heap and will not\n"
        "survive a reboot. Attach an IDE disk to keep them.\n"
        "\n"
        "Try: ls, cat readme.txt, write notes.txt hello, rm notes.txt\n";

    static const char about[] =
        "TazOS - a bare metal x86 kernel.\n"
        "Preemptive scheduling, paging, ring 3, PCI, a GUI.\n";

    const char *readme = persistent ? readme_disk : readme_ram;
    uint32_t rlen = persistent
        ? (uint32_t)(sizeof(readme_disk) - 1)
        : (uint32_t)(sizeof(readme_ram) - 1);

    fs_write("readme.txt", readme, rlen);
    fs_write("about.txt", about, (uint32_t)(sizeof(about) - 1));
}

void fs_init(void)
{
    memset(files, 0, sizeof(files));
    persist_enabled = false;
    on_disk = false;
    last_error = "";

    if (!ata_present()) {
        boot_state = "volatile (no disk)";
        seed_defaults(false);
        kprintf("filesystem       : volatile (no disk) - files live in RAM\n");
        return;
    }

    if (ata_sectors() < TAZFS_MIN_SECTORS) {
        boot_state = "volatile (disk too small)";
        seed_defaults(false);
        kprintf("filesystem       : volatile (disk too small, need %u sectors)\n",
                (uint32_t)TAZFS_MIN_SECTORS);
        return;
    }

    if (load_from_disk()) {
        on_disk = true;
        persist_enabled = true;
        boot_state = "loaded";
        kprintf("filesystem       : loaded %d file%s from disk\n",
                fs_file_count(), fs_file_count() == 1 ? "" : "s");
        return;
    }

    /* Blank, unknown magic, or a superblock whose contents do not add up.
     * Format rather than limp on with a half-read table. */
    free_all_data();
    persist_enabled = false;
    seed_defaults(true);
    persist_enabled = true;
    on_disk = true;

    if (!flush_all()) {
        persist_enabled = false;
        on_disk = false;
        boot_state = "volatile (disk write failed)";
        kprintf("filesystem       : volatile (format write failed)\n");
        return;
    }

    boot_state = "formatted";
    kprintf("filesystem       : formatted disk, %d files seeded\n",
            fs_file_count());
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

    last_error = "table full";
    return 0;
}

/* Make sure a file can hold `needed` bytes, reallocating if not.
 *
 * There is no realloc in this kernel, so growth is allocate-copy-free by
 * hand. Capacity is grown in steps rather than to the exact size, so
 * repeatedly appending a few bytes does not reallocate on every call. */
static bool ensure_capacity(fs_file_t *f, uint32_t needed)
{
    if (!grow_to(f, needed)) {
        last_error = (needed > FS_MAX_SIZE) ? "file too large" : "out of memory";
        return false;
    }
    return true;
}

static int slot_of(const fs_file_t *f)
{
    return (int)(f - files);
}

bool fs_write(const char *name, const void *data, uint32_t size)
{
    fs_file_t *f;

    last_error = "";
    f = fs_create(name);
    if (!f || !ensure_capacity(f, size))
        return false;

    if (size && data)
        memcpy(f->data, data, size);

    f->size = size;
    stamp(f);

    if (!flush_slot(slot_of(f))) {
        last_error = "disk write failed";
        return false;
    }
    return true;
}

bool fs_append(const char *name, const void *data, uint32_t size)
{
    fs_file_t *f;

    last_error = "";
    f = fs_create(name);
    if (!f || !ensure_capacity(f, f->size + size))
        return false;

    if (size && data)
        memcpy(f->data + f->size, data, size);

    f->size += size;
    stamp(f);

    if (!flush_slot(slot_of(f))) {
        last_error = "disk write failed";
        return false;
    }
    return true;
}

bool fs_delete(const char *name)
{
    fs_file_t *f;

    last_error = "";
    f = fs_find(name);
    if (!f)
        return false;

    if (f->data)
        kfree(f->data);

    memset(f, 0, sizeof(*f));

    if (!flush_slot(-1)) {
        last_error = "disk write failed";
        return false;
    }
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

const char *fs_error(void)
{
    return last_error ? last_error : "";
}

bool fs_on_disk(void)
{
    return on_disk;
}

void fs_defer_persist(bool defer)
{
    persist_defer = defer ? 1 : 0;
}

bool fs_flush_file(const char *name)
{
    fs_file_t *f = fs_find(name);
    int saved, slot;

    if (!f)
        return false;
    if (!persist_enabled)
        return true;

    saved = persist_defer;
    persist_defer = 0;
    slot = slot_of(f);
    {
        bool ok = flush_slot(slot);
        persist_defer = saved;
        return ok;
    }
}

const char *fs_boot_state(void)
{
    return boot_state;
}
