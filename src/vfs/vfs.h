#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "drivers/block/block.h"

typedef enum {
    VNODE_NONE = 0,
    VNODE_FILE = 1,
    VNODE_DIR  = 2,
} vnode_type_t;

typedef struct vfs_stat {
    vnode_type_t type;
    uint32_t ino;
    uint64_t size;
    uint32_t mode;
    uint32_t link_count;
    uint64_t mtime;
    uint64_t ctime;
} vfs_stat_t;

struct vnode;
struct vfs_mount;

#define VFS_MAX_MOUNT_PATH 256u

// Callback for readdir. Return false to stop iteration.
typedef bool (*vfs_readdir_cb)(const char* name, uint32_t ino, void* user);

typedef struct vnode_ops {
    bool    (*lookup)(struct vnode* dir, const char* name, struct vnode** out);
    bool    (*readdir)(struct vnode* dir, vfs_readdir_cb cb, void* user);
    int64_t (*read)(struct vnode* vn, uint64_t offset, void* buf, uint64_t len); // bytes read, or <0 on error
    int64_t (*write)(struct vnode* vn, uint64_t offset, const void* buf, uint64_t len); // bytes written, or <0 on error
    bool    (*truncate)(struct vnode* vn, uint64_t size);
    bool    (*create)(struct vnode* dir, const char* name, uint32_t mode, struct vnode** out);
    bool    (*mkdir)(struct vnode* dir, const char* name, uint32_t mode);
    bool    (*unlink)(struct vnode* dir, const char* name);
    bool    (*stat)(struct vnode* vn, vfs_stat_t* out);
    void    (*release)(struct vnode* vn);
} vnode_ops_t;

typedef struct vnode {
    vnode_type_t type;
    uint32_t ino;
    uint64_t size;

    const vnode_ops_t* ops;
    struct vfs_mount* mount;
    void* fs_private; // driver-owned vnode state
} vnode_t;

typedef struct vfs_mount {
    const char* fs_name;
    block_device_t* dev;
    vnode_t* root;
    char mount_path[VFS_MAX_MOUNT_PATH];

    bool readonly;
    void* fs_private; // driver-owned mount state
} vfs_mount_t;

// Initialize VFS registry (register builtin drivers, attempt no mount).
void vfs_init(void);

// Auto-mount root volume using probe order (KiFS -> FAT).
// Returns true on success.
bool vfs_mount_root_auto(void);

// Mount a specific device as root, using probe order.
bool vfs_mount_root_dev(block_device_t* dev);

// Mount a specific device at an absolute path, using probe order.
bool vfs_mount_dev(const char* abs_path, block_device_t* dev);

// Bind a preconstructed pseudo-filesystem mount at an absolute path.
bool vfs_bind_mount(const char* abs_path, vfs_mount_t* mount);

// Current root mount (NULL if none).
vfs_mount_t* vfs_root_mount(void);

// Returns the mount bound exactly at abs_path, if any.
vfs_mount_t* vfs_mount_at(const char* abs_path);

// Resolve an absolute path ("/..."), returning a newly-allocated vnode.
// Caller owns the vnode and must free it with vfs_vnode_put().
bool vfs_resolve(const char* abs_path, vnode_t** out);
bool vfs_readdir(const char* abs_path, vfs_readdir_cb cb, void* user);
bool vfs_create(const char* abs_path, uint32_t mode, vnode_t** out);
bool vfs_mkdir(const char* abs_path, uint32_t mode);
bool vfs_unlink(const char* abs_path);

// Drop a vnode allocated by VFS/driver.
void vfs_vnode_put(vnode_t* vn);
