#include "pci.h"
#include "io.h"
#include "console.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_VENDOR_NONE 0xFFFF

static pci_device_t devices[PCI_MAX_DEVICES];
static int          device_count;

/* The address written to 0xCF8 is a packed selector. Bit 31 enables the
 * access; the offset must be dword-aligned, which is why the low two bits are
 * masked off rather than shifted in. */
static uint32_t config_address(uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t offset)
{
    return (uint32_t)0x80000000
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)fn   << 8)
         | ((uint32_t)offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset,
                 uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, offset));
    outl(PCI_CONFIG_DATA, value);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t value = pci_read32(bus, dev, fn, offset);
    return (uint16_t)((value >> ((offset & 2) * 8)) & 0xFFFF);
}

static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t value = pci_read32(bus, dev, fn, offset);
    return (uint8_t)((value >> ((offset & 3) * 8)) & 0xFF);
}

uint32_t pci_bar_address(uint32_t bar)
{
    /* Memory BARs keep the address in bits 31:4, I/O BARs in bits 31:2. */
    return pci_bar_is_io(bar) ? (bar & 0xFFFFFFFC) : (bar & 0xFFFFFFF0);
}

bool pci_bar_is_io(uint32_t bar)
{
    return (bar & 1) != 0;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x00: return "unclassified";
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "mass storage";
        }
    case 0x02: return "network controller";
    case 0x03:
        return (subclass == 0x00) ? "VGA display" : "display controller";
    case 0x04: return "multimedia";
    case 0x05: return "memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input controller";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    default:   return "unknown";
    }
}

static void read_device(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (device_count >= PCI_MAX_DEVICES)
        return;

    pci_device_t *d = &devices[device_count];

    d->bus       = bus;
    d->device    = dev;
    d->function  = fn;
    d->vendor_id = pci_read16(bus, dev, fn, 0x00);
    d->device_id = pci_read16(bus, dev, fn, 0x02);
    d->revision  = pci_read8(bus, dev, fn, 0x08);
    d->prog_if   = pci_read8(bus, dev, fn, 0x09);
    d->subclass  = pci_read8(bus, dev, fn, 0x0A);
    d->class_code = pci_read8(bus, dev, fn, 0x0B);
    d->header_type = pci_read8(bus, dev, fn, 0x0E);
    d->irq_line  = pci_read8(bus, dev, fn, 0x3C);

    /* Only header type 0 has six BARs; bridges have two and a different
     * layout beyond that, so reading six would be reading other fields. */
    int bars = ((d->header_type & 0x7F) == 0x00) ? 6 : 2;

    for (int i = 0; i < 6; i++)
        d->bar[i] = (i < bars)
                  ? pci_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4))
                  : 0;

    device_count++;
}

void pci_init(void)
{
    device_count = 0;

    /* A brute-force sweep of every possible address.
     *
     * The tidy alternative is to walk the bridge topology recursively, which
     * is what a general-purpose kernel does. On a machine with a handful of
     * devices the exhaustive scan costs a few thousand port reads once at
     * boot, and it cannot miss a device sitting behind a bridge we failed to
     * follow. */
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            if (pci_read16((uint8_t)bus, dev, 0, 0x00) == PCI_VENDOR_NONE)
                continue;   /* nothing here at all; skip its functions */

            uint8_t header = pci_read8((uint8_t)bus, dev, 0, 0x0E);

            /* Bit 7 of the header type says the device is multi-function.
             * Probing all eight functions of a single-function device can
             * return aliased copies of function 0 on some hardware. */
            uint8_t functions = (header & 0x80) ? 8 : 1;

            for (uint8_t fn = 0; fn < functions; fn++) {
                if (pci_read16((uint8_t)bus, dev, fn, 0x00) == PCI_VENDOR_NONE)
                    continue;

                read_device((uint8_t)bus, dev, fn);
            }
        }
    }

    kprintf("pci              : %d device%s found\n",
            device_count, device_count == 1 ? "" : "s");
}

int pci_device_count(void)
{
    return device_count;
}

const pci_device_t *pci_get_device(int index)
{
    if (index < 0 || index >= device_count)
        return 0;

    return &devices[index];
}

const pci_device_t *pci_find(uint16_t vendor_id, uint16_t device_id)
{
    for (int i = 0; i < device_count; i++)
        if (devices[i].vendor_id == vendor_id &&
            devices[i].device_id == device_id)
            return &devices[i];

    return 0;
}
