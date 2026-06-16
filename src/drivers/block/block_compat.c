#include "drivers/block/block.h"
#include "libc/string.h"

/*
 * Compatibility exports for trees that have callers for the Phase 17 block
 * registry API before their block.c has been rebuilt or updated with the real
 * multi-disk implementations. Strong definitions in block.c override these.
 */

__attribute__((weak)) uint32_t block_rescan(void) {
    return 0;
}

__attribute__((weak)) uint32_t block_poll_hotplug(void) {
    return 0;
}

__attribute__((weak)) uint32_t block_disk_count(void) {
    return block_boot_device() ? 1u : 0u;
}

__attribute__((weak)) block_device_t* block_disk_device(uint32_t index) {
    return index == 0u ? block_boot_device() : 0;
}

__attribute__((weak)) block_device_t* block_device_by_name(const char* name) {
    block_device_t* boot = NULL;

    if (!name || !*name) {
        return 0;
    }

    boot = block_boot_device();
    if (boot && boot->name && strcmp(boot->name, name) == 0) {
        return boot;
    }

    for (uint32_t i = 0; i < block_partition_count(); i++) {
        block_device_t* part = block_partition_device(i);
        if (part && part->name && strcmp(part->name, name) == 0) {
            return part;
        }
    }

    return 0;
}

__attribute__((weak)) block_device_t* block_partition_lookup(uint32_t disk_index,
                                                            uint32_t part_number_one_based) {
    block_device_t* disk = NULL;

    if (disk_index != 0u || part_number_one_based == 0u) {
        return 0;
    }

    disk = block_boot_device();
    if (!disk) {
        return 0;
    }

    for (uint32_t i = 0; i < block_partition_count(); i++) {
        block_device_t* part = block_partition_device(i);
        if (i + 1u == part_number_one_based) {
            return part;
        }
    }

    return 0;
}

__attribute__((weak)) part_table_type_t block_disk_partition_table_type(uint32_t disk_index) {
    return disk_index == 0u ? block_partition_table_type() : PART_TABLE_NONE;
}
