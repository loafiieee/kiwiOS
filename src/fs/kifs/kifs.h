#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "drivers/block/block.h"
#include "vfs/vfs.h"

// VFS driver hooks
bool kifs_probe(block_device_t* dev);
bool kifs_mount(block_device_t* dev, vfs_mount_t** out_mount);

// Shell helper: format a device with KiFS v0.1.
// WARNING: this destroys all data on the device.
bool kifs_mkfs(block_device_t* dev, uint32_t inode_count);
bool kifs_mkfs_ex(block_device_t* dev, uint32_t inode_count, bool create_kiwios_root);

// Debug helper: read a window of bits from the on-disk block/inode bitmap.
// out_bits[i] is 0 or 1 for the requested bit range.
bool kifs_debug_get_bitmap_bits(const vfs_mount_t* m, bool inode_bitmap, uint32_t start_bit, uint32_t nbits, uint8_t* out_bits);

// Debug helper: copy the selected superblock from the mounted KiFS instance.
// Returns false if mount is not KiFS or not mounted.
bool kifs_debug_get_superblock(const vfs_mount_t* m, void* out_superblock_4k);
