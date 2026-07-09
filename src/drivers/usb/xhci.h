#pragma once

#include <stdint.h>

void xhci_probe_mmio(uint8_t bus, uint8_t dev, uint8_t func, uint64_t mmio_phys);
uint32_t xhci_storage_rescan(void);
uint32_t xhci_storage_rescan_force(void);
