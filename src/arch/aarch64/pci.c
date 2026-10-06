#include "pci.h"
#include <stdint.h>

/*
 * QEMU virt (AArch64) – PCI ECAM base clásica.
 * No se usa por defecto (vamos por virtio-mmio), pero queda listo.
 */
#define PCI_ECAM_BASE  0x3f000000ULL

static inline volatile uint32_t *ecam_addr(uint8_t bus, uint8_t slot,
                                          uint8_t function, uint8_t offset)
{
    return (volatile uint32_t *)(PCI_ECAM_BASE
        + ((uint64_t)bus << 20)
        + ((uint64_t)slot << 15)
        + ((uint64_t)function << 12)
        + (offset & 0xFFC));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    return *ecam_addr(bus, slot, function, offset);
}

uint16_t pci_vendor_id(uint8_t bus, uint8_t slot, uint8_t function)
{
    return (uint16_t)(pci_config_read32(bus, slot, function, 0) & 0xFFFF);
}

uint16_t pci_device_id(uint8_t bus, uint8_t slot, uint8_t function)
{
    return (uint16_t)(pci_config_read32(bus, slot, function, 0) >> 16);
}

uint32_t pci_bar0(uint8_t bus, uint8_t slot, uint8_t function)
{
    return pci_config_read32(bus, slot, function, 0x10);
}
