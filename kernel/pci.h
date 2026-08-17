/* pci.h — PCI bus enumeration.
 *
 * Finding out what hardware is present is one of the things an operating
 * system exists to do. Up to this point every device in this kernel has been
 * at a fixed legacy address that has not moved since 1981 — the PIC at 0x20,
 * the timer at 0x40, the keyboard controller at 0x60. Anything newer than
 * that has to be discovered, because its addresses are assigned at boot by
 * the firmware and differ between machines.
 *
 * PCI configuration space is reached through two I/O ports: write which
 * bus/device/function/offset you want to one, then read or write the value
 * through the other.
 */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_MAX_DEVICES 32

typedef struct {
    uint8_t  bus, device, function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;

    uint8_t  irq_line;

    /* Base address registers. The low bits are flags, not address: bit 0
     * distinguishes an I/O port range from a memory range. */
    uint32_t bar[6];
} pci_device_t;

void pci_init(void);

int                 pci_device_count(void);
const pci_device_t *pci_get_device(int index);

/* Find the first device matching a vendor and device ID, or NULL. */
const pci_device_t *pci_find(uint16_t vendor_id, uint16_t device_id);

/* Raw configuration space access. */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset,
                     uint32_t value);

/* Human-readable name for a class/subclass pair. */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* Strip the flag bits to get the usable base address. */
uint32_t pci_bar_address(uint32_t bar);
bool     pci_bar_is_io(uint32_t bar);

#endif /* PCI_H */
