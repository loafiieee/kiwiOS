#pragma once
#include <stdint.h>
#include <stdbool.h>

void ahci_probe_mmio(uint32_t mmio_phys);
bool ahci_disk_ready(void);
uint32_t ahci_disk_count(void);
uint64_t ahci_disk_total_sectors(uint32_t index);
bool ahci_disk_read_index(uint32_t index, uint64_t lba, uint32_t sector_count, void* buffer);
bool ahci_disk_write_index(uint32_t index, uint64_t lba, uint32_t sector_count, const void* buffer);
bool ahci_disk_flush_index(uint32_t index);

// Read/write 512-byte sectors from the first detected SATA disk.
bool ahci_read(uint64_t lba, uint32_t sector_count, void* buffer);
bool ahci_write(uint64_t lba, uint32_t sector_count, const void* buffer);

// Force drive write cache to be committed (needed for journaling correctness).
bool ahci_flush(void);
