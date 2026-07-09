#include "fs/devfs/devfs.h"
#include "drivers/block/block.h"
#include "memory/heap.h"
#include "libc/string.h"
#include "vfs/vfs.h"

typedef enum {
    DEVFS_NODE_ROOT = 0,
    DEVFS_NODE_CONSOLE,
    DEVFS_NODE_BLOCK,
} devfs_node_kind_t;

typedef struct {
    devfs_node_kind_t kind;
    uint32_t ino;
    block_device_t* block_dev;
} devfs_node_t;

typedef struct {
    devfs_node_t root_node;
} devfs_fs_t;

static const vnode_ops_t g_devfs_vops;

static devfs_fs_t* devfs_fs(vnode_t* vn) {
    if (!vn || !vn->mount || !vn->mount->fs_private) {
        return NULL;
    }
    return (devfs_fs_t*)vn->mount->fs_private;
}

static devfs_node_t* devfs_node(vnode_t* vn) {
    return vn ? (devfs_node_t*)vn->fs_private : NULL;
}

static bool devfs_make_root_alias(vnode_t* dir, vnode_t** out) {
    vnode_t* vn = NULL;

    if (!dir || !dir->mount || !dir->mount->root || !out) {
        return false;
    }

    vn = (vnode_t*)kmalloc(sizeof(vnode_t));
    if (!vn) {
        return false;
    }

    *vn = *dir->mount->root;
    vn->refcount = 1;
    *out = vn;
    return true;
}

static bool devfs_alloc_file(vnode_t* dir,
                             uint32_t ino,
                             devfs_node_kind_t kind,
                             block_device_t* block_dev,
                             vnode_t** out) {
    vnode_t* vn = NULL;
    devfs_node_t* node = NULL;

    if (!dir || !dir->mount || !out) {
        return false;
    }

    *out = NULL;

    vn = (vnode_t*)kmalloc(sizeof(vnode_t));
    node = (devfs_node_t*)kmalloc(sizeof(devfs_node_t));
    if (!vn || !node) {
        if (vn) kfree(vn);
        if (node) kfree(node);
        return false;
    }

    memset(vn, 0, sizeof(*vn));
    memset(node, 0, sizeof(*node));

    node->kind = kind;
    node->ino = ino;
    node->block_dev = block_dev;

    vn->type = VNODE_FILE;
    vn->ino = ino;
    vn->size = 0;
    vn->ops = &g_devfs_vops;
    vn->mount = dir->mount;
    vn->fs_private = node;

    *out = vn;
    return true;
}

static bool devfs_lookup_block_name(const char* name, uint32_t* out_ino, block_device_t** out_dev) {
    uint32_t disk_count = 0;
    uint32_t part_count = 0;

    if (!name || !out_ino || !out_dev) {
        return false;
    }

    (void)block_poll_hotplug();

    disk_count = block_disk_count();
    for (uint32_t i = 0; i < disk_count; i++) {
        block_device_t* dev = block_disk_device(i);
        if (!dev || !dev->name || !block_device_is_present(dev)) {
            continue;
        }
        if (strcmp(dev->name, name) == 0) {
            *out_ino = 0x100u + i;
            *out_dev = dev;
            return true;
        }
    }

    part_count = block_partition_count();
    for (uint32_t i = 0; i < part_count; i++) {
        block_device_t* dev = block_partition_device(i);
        if (!dev || !dev->name || !block_device_is_present(dev)) {
            continue;
        }
        if (strcmp(dev->name, name) == 0) {
            *out_ino = 0x200u + i;
            *out_dev = dev;
            return true;
        }
    }

    return false;
}

static bool devfs_vnode_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    block_device_t* dev = NULL;
    uint32_t ino = 0;

    if (!dir || dir->type != VNODE_DIR || !name || !out) {
        return false;
    }

    *out = NULL;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return devfs_make_root_alias(dir, out);
    }

    if (strcmp(name, "console") == 0) {
        return devfs_alloc_file(dir, 2u, DEVFS_NODE_CONSOLE, NULL, out);
    }

    if (devfs_lookup_block_name(name, &ino, &dev)) {
        return devfs_alloc_file(dir, ino, DEVFS_NODE_BLOCK, dev, out);
    }

    return false;
}

static bool devfs_vnode_readdir(vnode_t* dir, vfs_readdir_cb cb, void* user) {
    uint32_t disk_count = 0;
    uint32_t part_count = 0;

    if (!dir || dir->type != VNODE_DIR || !cb) {
        return false;
    }

    (void)block_poll_hotplug();

    if (!cb("console", 2u, user)) {
        return true;
    }

    disk_count = block_disk_count();
    for (uint32_t i = 0; i < disk_count; i++) {
        block_device_t* dev = block_disk_device(i);
        if (!dev || !dev->name || !block_device_is_present(dev)) {
            continue;
        }
        if (!cb(dev->name, 0x100u + i, user)) {
            return true;
        }
    }

    part_count = block_partition_count();
    for (uint32_t i = 0; i < part_count; i++) {
        block_device_t* dev = block_partition_device(i);
        if (!dev || !dev->name || !block_device_is_present(dev)) {
            continue;
        }
        if (!cb(dev->name, 0x200u + i, user)) {
            return true;
        }
    }

    return true;
}

static bool devfs_vnode_stat(vnode_t* vn, vfs_stat_t* out) {
    devfs_node_t* node = NULL;

    if (!vn || !out) {
        return false;
    }

    node = devfs_node(vn);
    if (!node) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->type = vn->type;
    out->ino = vn->ino;
    out->size = 0;
    out->mode = (vn->type == VNODE_DIR) ? 0555u : 0444u;
    out->link_count = (vn->type == VNODE_DIR) ? 2u : 1u;
    return true;
}

static void devfs_vnode_release(vnode_t* vn) {
    devfs_fs_t* fs = NULL;
    devfs_node_t* node = NULL;

    if (!vn) {
        return;
    }

    fs = devfs_fs(vn);
    node = devfs_node(vn);
    if (!fs || !node) {
        return;
    }

    if (node != &fs->root_node) {
        kfree(node);
    }
}

static const vnode_ops_t g_devfs_vops = {
    .lookup = devfs_vnode_lookup,
    .readdir = devfs_vnode_readdir,
    .stat = devfs_vnode_stat,
    .release = devfs_vnode_release,
};

bool devfs_mount_at(const char* abs_path) {
    devfs_fs_t* fs = NULL;
    vnode_t* root = NULL;
    vfs_mount_t* mount = NULL;

    fs = (devfs_fs_t*)kmalloc(sizeof(devfs_fs_t));
    root = (vnode_t*)kmalloc(sizeof(vnode_t));
    mount = (vfs_mount_t*)kmalloc(sizeof(vfs_mount_t));
    if (!fs || !root || !mount) {
        if (fs) kfree(fs);
        if (root) kfree(root);
        if (mount) kfree(mount);
        return false;
    }

    memset(fs, 0, sizeof(*fs));
    memset(root, 0, sizeof(*root));
    memset(mount, 0, sizeof(*mount));

    fs->root_node.kind = DEVFS_NODE_ROOT;
    fs->root_node.ino = 1u;
    fs->root_node.block_dev = NULL;

    root->type = VNODE_DIR;
    root->ino = 1u;
    root->size = 0;
    root->ops = &g_devfs_vops;
    root->mount = mount;
    root->fs_private = &fs->root_node;

    mount->fs_name = "devfs";
    mount->dev = NULL;
    mount->root = root;
    mount->readonly = true;
    mount->fs_private = fs;

    if (!vfs_bind_mount(abs_path, mount)) {
        kfree(mount);
        kfree(root);
        kfree(fs);
        return false;
    }

    return true;
}
