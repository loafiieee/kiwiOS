#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct block_device block_device_t;

typedef bool (*block_read_fn)(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer);
typedef bool (*block_write_fn)(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer);
typedef bool (*block_flush_fn)(block_device_t* dev);

typedef enum {
    PART_TABLE_NONE = 0,
    PART_TABLE_MBR  = 1,
    PART_TABLE_GPT  = 2
} part_table_type_t;

struct block_device {
    const char* name;
    uint32_t sector_size;     // usually 512

    // Total size (in sectors) if known; 0 if unknown.
    uint64_t total_sectors;

    // Driver-private pointer (for partitions/wrappers).
    void* ctx;

    block_read_fn  read;
    block_write_fn write;
    block_flush_fn flush;     // may be NULL if unsupported
};

// Build the block-device registry from the currently discovered AHCI disks,
// then probe partitions (GPT first, then MBR fallback).
void block_init(void);

// Rescan PCI/AHCI/USB for newly appeared disks and append them to the block registry.
// Returns the number of newly registered whole disks.
uint32_t block_rescan(void);

// Cheap hotplug poll for already registered hotplug-capable controllers.
// This intentionally avoids a full PCI rescan so it can run from idle paths.
uint32_t block_poll_hotplug(void);

// Returns the first registered boot block device, or NULL if none.
block_device_t* block_boot_device(void);

// Whole-disk enumeration.
uint32_t block_disk_count(void);
block_device_t* block_disk_device(uint32_t index);
block_device_t* block_device_by_name(const char* name);

// Partition enumeration
uint32_t block_partition_count(void);
block_device_t* block_partition_device(uint32_t index);
block_device_t* block_partition_lookup(uint32_t disk_index, uint32_t part_number_one_based);

// Returns which partition table was detected on the boot disk.
part_table_type_t block_partition_table_type(void);
part_table_type_t block_disk_partition_table_type(uint32_t disk_index);
