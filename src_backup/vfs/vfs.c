#include "vfs/vfs.h"
#include "core/log.h"
#include "memory/heap.h"
#include "libc/string.h"

// --- FS driver interface (internal) ---

typedef struct fs_driver {
    const char* name;
    bool (*probe)(block_device_t* dev);
    bool (*mount)(block_device_t* dev, vfs_mount_t** out_mount);
} fs_driver_t;

// Forward decls for built-in drivers
bool kifs_probe(block_device_t* dev);
bool kifs_mount(block_device_t* dev, vfs_mount_t** out_mount);

bool fat_probe(block_device_t* dev);
bool fat_mount(block_device_t* dev, vfs_mount_t** out_mount);

static const fs_driver_t g_drivers[] = {
    { "kifs", kifs_probe, kifs_mount },
    { "fat",  fat_probe,  fat_mount  },
};

static vfs_mount_t* g_root_mount = NULL;

static block_device_t* pick_default_root_device(void) {
    uint32_t pc = block_partition_count();
    if (pc > 0) {
        return block_partition_device(0);
    }
    return block_boot_device();
}

void vfs_init(void) {
    g_root_mount = NULL;
    log_ok("vfs", "VFS initialized");
}

vfs_mount_t* vfs_root_mount(void) {
    return g_root_mount;
}

static bool mount_with_driver(const fs_driver_t* d, block_device_t* dev) {
    vfs_mount_t* m = NULL;
    if (!d->mount(dev, &m) || !m) {
        return false;
    }

    // Replace existing root mount (leak-safe: this OS has no unmount yet; free minimal).
    g_root_mount = m;

    log_okf("vfs", "Mounted %s on %s (%s)",
            m->fs_name ? m->fs_name : d->name,
            dev && dev->name ? dev->name : "(noname)",
            m->readonly ? "ro" : "rw");
    return true;
}

bool vfs_mount_root_dev(block_device_t* dev) {
    if (!dev) return false;

    for (uint32_t i = 0; i < (sizeof(g_drivers) / sizeof(g_drivers[0])); i++) {
        const fs_driver_t* d = &g_drivers[i];
        if (!d->probe || !d->mount) continue;

        if (d->probe(dev)) {
            log_infof("vfs", "Probe matched: %s on %s", d->name, dev->name ? dev->name : "(noname)");
            if (mount_with_driver(d, dev)) return true;
            log_errorf("vfs", "Mount failed for driver %s", d->name);
        }
    }

    log_error("vfs", "No supported filesystem detected");
    return false;
}

bool vfs_mount_root_auto(void) {
    block_device_t* dev = pick_default_root_device();
    if (!dev) {
        log_error("vfs", "No block device available for root mount");
        return false;
    }
    return vfs_mount_root_dev(dev);
}

static bool is_abs_path(const char* p) {
    return p && p[0] == '/';
}

static const char* next_component(const char* p, char* out, uint32_t out_cap) {
    // p points at start of component or '/'.
    while (*p == '/') p++;
    if (*p == 0) {
        out[0] = 0;
        return p;
    }

    uint32_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 < out_cap) {
            out[n++] = *p;
        }
        p++;
    }
    out[n] = 0;
    return p;
}

bool vfs_resolve(const char* abs_path, vnode_t** out) {
    if (!out) return false;
    *out = NULL;

    if (!is_abs_path(abs_path)) {
        return false;
    }

    if (!g_root_mount || !g_root_mount->root) {
        return false;
    }

    // Special case: "/"
    if (strcmp(abs_path, "/") == 0) {
        // Duplicate root vnode by asking driver to resolve inode again if possible.
        // For now: shallow copy of vnode struct (safe because fs_private is immutable for v0.1).
        vnode_t* r = (vnode_t*)kmalloc(sizeof(vnode_t));
        if (!r) return false;
        *r = *g_root_mount->root;
        *out = r;
        return true;
    }

    vnode_t* cur = (vnode_t*)kmalloc(sizeof(vnode_t));
    if (!cur) return false;
    *cur = *g_root_mount->root;

    const char* p = abs_path;
    char name[256];

    while (1) {
        p = next_component(p, name, sizeof(name));
        if (name[0] == 0) break;

        if (!cur->ops || !cur->ops->lookup) {
            vfs_vnode_put(cur);
            return false;
        }
        if (cur->type != VNODE_DIR) {
            vfs_vnode_put(cur);
            return false;
        }

        vnode_t* next = NULL;
        if (!cur->ops->lookup(cur, name, &next) || !next) {
            vfs_vnode_put(cur);
            return false;
        }

        vfs_vnode_put(cur);
        cur = next;
    }

    *out = cur;
    return true;
}

void vfs_vnode_put(vnode_t* vn) {
    if (!vn) return;
    // In v0.1 we allocate vnodes with kmalloc and do not maintain refcounts.
    kfree(vn);
}

