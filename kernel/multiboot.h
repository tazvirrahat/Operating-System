/* multiboot.h — the boot information GRUB leaves for us.
 *
 * Only the fields this kernel reads are named; the rest are padding held at
 * their fixed offsets, because the structure layout is fixed by the standard
 * and cannot be trimmed.
 */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Bits in the flags word saying which fields are valid. */
#define MB_INFO_MEMORY      (1 << 0)
#define MB_INFO_FRAMEBUFFER (1 << 12)

#define MB_FRAMEBUFFER_RGB     1
#define MB_FRAMEBUFFER_TEXT    2

typedef struct {
    uint32_t flags;

    uint32_t mem_lower;         /* KB below 1 MB */
    uint32_t mem_upper;         /* KB above 1 MB */

    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    /* Valid only when MB_INFO_FRAMEBUFFER is set in flags. */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;    /* bytes per scanline, NOT width * bytes */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;

    /* For an RGB framebuffer these six bytes are three (position, size) pairs.
     * The position is the first byte of each pair, not the second — reading
     * the size by mistake gives 8 for every channel, which packs red, green
     * and blue on top of one another and renders the entire display in
     * shades of green. */
    uint8_t  red_position;
    uint8_t  red_mask_size;
    uint8_t  green_position;
    uint8_t  green_mask_size;
    uint8_t  blue_position;
    uint8_t  blue_mask_size;
} __attribute__((packed)) multiboot_info_t;

#endif /* MULTIBOOT_H */
