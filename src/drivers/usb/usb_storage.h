#pragma once
#include <stdbool.h>
#include <stdint.h>

void uhci_probe_pci(uint8_t bus, uint8_t dev, uint8_t func, uint16_t io_base);

uint32_t usb_storage_rescan(void);
uint32_t usb_storage_disk_count(void);
uint64_t usb_storage_total_sectors(uint32_t index);
bool usb_storage_read(uint32_t index, uint64_t lba, uint32_t sector_count, void* buffer);
bool usb_storage_write(uint32_t index, uint64_t lba, uint32_t sector_count, const void* buffer);
bool usb_storage_flush(uint32_t index);
