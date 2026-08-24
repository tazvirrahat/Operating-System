/* ata.h — ATA/IDE PIO on the legacy primary channel.
 *
 * The controller sits at the same ports it has since the original PC
 * (0x1F0), so it does not need PCI discovery. What it does need is the
 * same caution as the serial driver: an empty bus floats to 0xFF and will
 * look permanently busy if the wait is unbounded. Probe, then bound every
 * poll.
 *
 * 28-bit LBA, 512-byte sectors, polling rather than IRQ 14. This workload
 * is a handful of sectors at a time; an interrupt-driven driver would
 * exist to overlap I/O with other work, which we do not have yet.
 */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

#define ATA_SECTOR_SIZE 512

void        ata_init(void);
bool        ata_present(void);
const char *ata_model(void);
uint32_t    ata_sectors(void);
uint32_t    ata_writes(void);

/* One sector. lba is 28-bit; returns false if there is no drive, the LBA
 * is out of range, or the wait for BSY/DRQ timed out. */
bool ata_read(uint32_t lba, void *buf);
bool ata_write(uint32_t lba, const void *buf);

#endif /* ATA_H */
