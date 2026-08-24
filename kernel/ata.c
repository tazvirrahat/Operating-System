#include "ata.h"
#include "io.h"
#include "console.h"
#include "string.h"

/* Primary bus, master only. The CD-ROM in both QEMU and the VMware VM sits
 * on the secondary channel (0x170); putting the writable disk on primary
 * master is what makes this driver see it without also having to speak
 * ATAPI. */
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALTSTATUS  0x3F6

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_DRIVE_MASTER_LBA 0xE0
#define ATA_DRIVE_MASTER     0xA0

/* nIEN: disable IRQ 14. We poll, and a completion interrupt with no handler
 * registered would only be a spurious IRQ during later `sti`. */
#define ATA_NIEN 0x02

/* Interrupts are still masked when this driver comes up, so pit_ticks()
 * cannot bound the wait — the counter would never move. A spin limit is
 * the same defence the serial driver uses against a floating bus. */
#define ATA_SPIN_LIMIT 2000000u

static bool     present;
static char     model[41];
static uint32_t sectors;
static uint32_t write_count;

static uint8_t status(void)
{
    return inb(ATA_STATUS);
}

static bool wait_not_busy(void)
{
    for (uint32_t i = 0; i < ATA_SPIN_LIMIT; i++) {
        uint8_t st = status();
        if (st == 0xFF)
            return false;
        if ((st & ATA_SR_BSY) == 0)
            return true;
    }
    return false;
}

static bool wait_drq(void)
{
    for (uint32_t i = 0; i < ATA_SPIN_LIMIT; i++) {
        uint8_t st = status();
        if (st == 0xFF)
            return false;
        if (st & ATA_SR_ERR)
            return false;
        if (st & ATA_SR_DF)
            return false;
        if ((st & ATA_SR_BSY) == 0 && (st & ATA_SR_DRQ))
            return true;
    }
    return false;
}

static void select_master(uint8_t head_byte)
{
    outb(ATA_DRIVE, head_byte);
    io_wait();
    io_wait();
    io_wait();
    io_wait();
}

static void decode_ata_string(char *dst, const uint16_t *src, int words)
{
    int n = 0;

    for (int i = 0; i < words; i++) {
        char a = (char)(src[i] >> 8);
        char b = (char)(src[i] & 0xFF);
        if (a)
            dst[n++] = a;
        if (b)
            dst[n++] = b;
    }

    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0'))
        n--;
    dst[n] = '\0';
}

static bool identify(void)
{
    uint16_t id[256];

    outb(ATA_ALTSTATUS, ATA_NIEN);

    select_master(ATA_DRIVE_MASTER);

    uint8_t st = status();
    /* Empty socket, or a bus with nothing driving it. 0xFF is the floating
     * value the serial driver already documented; 0x00 is what QEMU reports
     * when the primary master slot has no drive at all. */
    if (st == 0xFF || st == 0x00)
        return false;

    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    st = status();
    if (st == 0x00 || st == 0xFF)
        return false;

    if (!wait_not_busy())
        return false;

    /* ATAPI devices (the CD-ROM) abort IDENTIFY and leave a signature in
     * the LBA mid/high registers. They are not a disk we can write. */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0)
        return false;

    st = status();
    if (st & (ATA_SR_ERR | ATA_SR_DF))
        return false;

    if (!wait_drq())
        return false;

    insw(ATA_DATA, id, 256);

    decode_ata_string(model, &id[27], 20);

    sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (sectors == 0)
        return false;

    return true;
}

static bool issue_lba28(uint32_t lba, uint8_t cmd)
{
    if (!present || lba >= sectors || (lba & ~0x0FFFFFFFu))
        return false;

    if (!wait_not_busy())
        return false;

    select_master((uint8_t)(ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, cmd);

    return wait_drq();
}

void ata_init(void)
{
    present = false;
    model[0] = '\0';
    sectors = 0;
    write_count = 0;

    if (!identify()) {
        kprintf("ata              : no drive on primary IDE (0x1F0)\n");
        return;
    }

    present = true;
    kprintf("ata              : %s, %u MB (%u sectors)\n",
            model[0] ? model : "(unnamed)",
            sectors / 2048u, sectors);
}

bool ata_present(void)
{
    return present;
}

const char *ata_model(void)
{
    return model;
}

uint32_t ata_sectors(void)
{
    return sectors;
}

uint32_t ata_writes(void)
{
    return write_count;
}

bool ata_read(uint32_t lba, void *buf)
{
    if (!buf || !issue_lba28(lba, ATA_CMD_READ_PIO))
        return false;

    insw(ATA_DATA, buf, 256);
    return wait_not_busy();
}

bool ata_write(uint32_t lba, const void *buf)
{
    if (!buf || !issue_lba28(lba, ATA_CMD_WRITE_PIO))
        return false;

    outsw(ATA_DATA, buf, 256);
    if (!wait_not_busy())
        return false;

    write_count++;
    return true;
}
