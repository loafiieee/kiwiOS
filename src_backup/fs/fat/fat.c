#include <stdbool.h>
#include <stddef.h>
#include "drivers/block/block.h"
#include "vfs/vfs.h"

// Placeholder FAT driver (Phase 8 in your checklist).
// For now, probing always returns false so KiFS takes precedence.

bool fat_probe(block_device_t* dev) {
    (void)dev;
    return false;
}

bool fat_mount(block_device_t* dev, vfs_mount_t** out_mount) {
    (void)dev;
    if (out_mount) *out_mount = NULL;
    return false;
}
