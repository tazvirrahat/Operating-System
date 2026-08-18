/* fs.h — an in-memory filesystem.
 *
 * Files live in the kernel heap rather than on a disk. There is no storage
 * driver in this kernel -- reaching a real disk would mean an ATA or AHCI
 * driver and then a partition and on-disk format on top of it -- so nothing
 * here survives a reboot, and the name says so.
 *
 * What it does provide is the part that is actually a filesystem: a namespace
 * mapping names to contents, allocation and release of the space those
 * contents occupy, metadata about each file, and the open/read/write/close
 * cycle that callers expect. Those are the same concerns a disk-backed
 * filesystem has; only the block layer underneath is missing.
 *
 * The namespace is flat. Directories would mean path parsing and a tree walk
 * for every lookup, and they do not demonstrate anything the flat version
 * does not.
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

/* Replace a file's contents. Grows the allocation if needed. */
bool fs_write(const char *name, const void *data, uint32_t size);

/* Append to a file, creating it if absent. */
bool fs_append(const char *name, const void *data, uint32_t size);

bool fs_delete(const char *name);

/* Iterate: index 0..FS_MAX_FILES-1, skipping unused slots. */
const fs_file_t *fs_at(int index);

int      fs_file_count(void);
uint32_t fs_bytes_used(void);

#endif /* FS_H */
