#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile (
        "inl %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

uint32_t pci_config_read32(
    uint8_t bus,
    uint8_t slot,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t address =
        (1U << 31) |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);

    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_vendor_id(
    uint8_t bus,
    uint8_t slot,
    uint8_t function
)
{
    uint32_t value =
        pci_config_read32(
            bus,
            slot,
            function,
            0x00
        );

    return (uint16_t)(value & 0xFFFF);
}

uint16_t pci_device_id(
    uint8_t bus,
    uint8_t slot,
    uint8_t function
)
{
    uint32_t value =
        pci_config_read32(
            bus,
            slot,
            function,
            0x00
        );

    return (uint16_t)((value >> 16) & 0xFFFF);
}
uint32_t pci_bar0(
    uint8_t bus,
    uint8_t slot,
    uint8_t function
)
{
    return pci_config_read32(
        bus,
        slot,
        function,
        0x10
    );
}