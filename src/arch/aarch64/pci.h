#ifndef MYOS_PCI_H
#define MYOS_PCI_H

#include <stdint.h>

uint32_t pci_config_read32(
    uint8_t bus,
    uint8_t slot,
    uint8_t function,
    uint8_t offset
);

uint16_t pci_vendor_id(
    uint8_t bus,
    uint8_t slot,
    uint8_t function
);

uint16_t pci_device_id(
    uint8_t bus,
    uint8_t slot,
    uint8_t function
);

uint32_t pci_bar0(
    uint8_t bus,
    uint8_t slot,
    uint8_t function
);

#endif