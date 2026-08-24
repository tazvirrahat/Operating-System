/* fs.h — a flat namespace, optionally backed by an ATA disk.
 *
 * The in-memory table is the filesystem: names map to contents, allocations
 * grow and shrink, timestamps come from the RTC. The disk is a write-through
 * copy of that table so the same files come back after a reboot.
 *
 * There is no directory tree and no general-purpose on-disk format. The
 * layout is sized for this table (64 files, 64 KB each) and nothing else.
 * If no drive answers IDENTIFY, behaviour is exactly the original RAM-only
 * kernel: files live in the heap and vanish on reset.
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stdbool.h>

#define FS_MAX_FILES 64
#define FS_NAME_MAX  32
#define FS_MAX_SIZE  (64 * 1024)

typedef struct {
    char     name[FS_NAME_MAX];
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    bool     used;

    /* Taken from the real-time clock at creation, which is why the clock
     * driver had to exist before this could report anything meaningful. */
    uint8_t  hour, minute;
    uint8_t  day, month;
    uint16_t year;
} fs_file_t;

void fs_init(void);

/* Create an empty file, or return the existing one of that name. */
fs_file_t *fs_create(const char *name);

fs_file_t *fs_find(const char *name);

/* Replace a file's contents. Grows the allocation if needed. On a disk
 * this is write-through: returning true means the bytes are on the drive,
 * not only in RAM. */
bool fs_write(const char *name, const void *data, uint32_t size);

/* Append to a file, creating it if absent. */
bool fs_append(const char *name, const void *data, uint32_t size);

bool fs_delete(const char *name);

/* Iterate: index 0..FS_MAX_FILES-1, skipping unused slots. */
const fs_file_t *fs_at(int index);

int      fs_file_count(void);
uint32_t fs_bytes_used(void);

/* Last mutating call's failure, or an empty string. */
const char *fs_error(void);

bool        fs_on_disk(void);
const char *fs_boot_state(void);

/* Pause write-through so a burst of tiny appends is one disk write. The
 * in-memory file stays live (File Explorer and Notepad read it). Nested
 * calls are not supported. */
void fs_defer_persist(bool defer);
bool fs_flush_file(const char *name);

#endif /* FS_H */
