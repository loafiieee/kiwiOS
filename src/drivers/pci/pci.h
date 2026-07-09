#pragma once
#include <stdint.h>

void pci_enumerate_and_log(void);
uint32_t pci_read_bar32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t value);

// Enable PCI bus mastering for a device function (required for DMA, including AHCI).
void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);
void pci_enable_io_bus_master(uint8_t bus, uint8_t dev, uint8_t func);
