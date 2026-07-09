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

#define VFS_MAX_MOUNTS 16u

typedef struct {
    bool used;
    vfs_mount_t* mount;
} vfs_mount_slot_t;

static vfs_mount_slot_t g_mounts[VFS_MAX_MOUNTS];

static bool mount_available(vfs_mount_t* mount) {
    if (!mount || !mount->root) {
        return false;
    }
    if (mount->dev && !block_device_is_present(mount->dev)) {
        return false;
    }
    return true;
}

static bool root_candidate(block_device_t* dev, bool require_kifs) {
    if (!dev || !block_device_is_present(dev) || block_device_is_removable(dev)) {
        return false;
    }
    return !require_kifs || kifs_probe(dev);
}

static block_device_t* pick_default_root_device(void) {
    uint32_t pc = block_partition_count();

    for (uint32_t i = 0; i < pc; i++) {
        block_device_t* dev = block_partition_device(i);
        if (root_candidate(dev, true)) {
            return dev;
        }
    }

    if (root_candidate(block_boot_device(), true)) {
        return block_boot_device();
    }

    for (uint32_t i = 0; i < pc; i++) {
        block_device_t* dev = block_partition_device(i);
        if (root_candidate(dev, false)) {
            return dev;
        }
    }

    if (root_candidate(block_boot_device(), false)) {
        return block_boot_device();
    }

    return NULL;
}

void vfs_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    log_ok("vfs", "VFS initialized");
}

static bool normalize_mount_path(const char* in, char* out, uint32_t out_cap) {
    uint32_t len = 0;

    if (!in || !out || out_cap < 2u || in[0] != '/') {
        return false;
    }

    len = (uint32_t)strlen(in);
    while (len > 1u && in[len - 1u] == '/') {
        len--;
    }

    if ((len + 1u) > out_cap) {
        return false;
    }

    memcpy(out, in, len);
    out[len] = '\0';
    return true;
}

static int find_mount_slot_exact(const char* normalized_path) {
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used || !g_mounts[i].mount) {
            continue;
        }
        if (strcmp(g_mounts[i].mount->mount_path, normalized_path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_mount_slot_available(void) {
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) {
            return (int)i;
        }
    }

    return -1;
}

static bool bind_mount_normalized(const char* normalized_path, vfs_mount_t* mount) {
    int slot = 0;
    int existing = 0;

    if (!normalized_path || !mount || !mount->root) {
        return false;
    }

    existing = find_mount_slot_exact(normalized_path);
    if (existing >= 0) {
        if (mount_available(g_mounts[existing].mount)) {
            log_errorf("vfs", "Mount point already in use: %s", normalized_path);
            return false;
        }
        g_mounts[existing].used = false;
        g_mounts[existing].mount = NULL;
    }

    slot = find_mount_slot_available();
    if (slot < 0) {
        log_errorf("vfs", "No free mount slots for %s", normalized_path);
        return false;
    }

    memset(mount->mount_path, 0, sizeof(mount->mount_path));
    memcpy(mount->mount_path, normalized_path, strlen(normalized_path) + 1u);
    mount->root->mount = mount;

    g_mounts[slot].used = true;
    g_mounts[slot].mount = mount;

    log_okf("vfs", "Mounted %s on %s at %s (%s)",
            mount->fs_name ? mount->fs_name : "(unknown)",
            mount->dev && mount->dev->name ? mount->dev->name : "(pseudo)",
            mount->mount_path,
            mount->readonly ? "ro" : "rw");
    return true;
}

bool vfs_bind_mount(const char* abs_path, vfs_mount_t* mount) {
    char normalized[VFS_MAX_MOUNT_PATH];

    if (!normalize_mount_path(abs_path, normalized, sizeof(normalized))) {
        return false;
    }

    return bind_mount_normalized(normalized, mount);
}

vfs_mount_t* vfs_mount_at(const char* abs_path) {
    char normalized[VFS_MAX_MOUNT_PATH];
    int slot = 0;

    if (!normalize_mount_path(abs_path, normalized, sizeof(normalized))) {
        return NULL;
    }

    slot = find_mount_slot_exact(normalized);
    if (slot < 0) {
        return NULL;
    }

    if (!mount_available(g_mounts[slot].mount)) {
        return NULL;
    }

    return g_mounts[slot].mount;
}

vfs_mount_t* vfs_root_mount(void) {
    return vfs_mount_at("/");
}

static bool mount_with_driver_at(const fs_driver_t* d,
                                 const char* normalized_path,
                                 block_device_t* dev) {
    vfs_mount_t* m = NULL;

    if (!d->mount(dev, &m) || !m) {
        return false;
    }

    return bind_mount_normalized(normalized_path, m);
}

bool vfs_mount_dev(const char* abs_path, block_device_t* dev) {
    char normalized[VFS_MAX_MOUNT_PATH];
    vnode_t* target = NULL;

    if (!dev) return false;
    if (!block_device_is_present(dev)) {
        log_errorf("vfs", "Mount source %s is not present", dev->name ? dev->name : "(noname)");
        return false;
    }
    if (!normalize_mount_path(abs_path, normalized, sizeof(normalized))) {
        return false;
    }
    if (strcmp(normalized, "/") != 0) {
        if (!vfs_resolve(normalized, &target) || !target || target->type != VNODE_DIR) {
            if (target) {
                vfs_vnode_put(target);
            }
            log_errorf("vfs", "Mount target %s is not an existing directory", normalized);
            return false;
        }
        vfs_vnode_put(target);
    }
    {
        int existing = find_mount_slot_exact(normalized);
        if (existing >= 0 && mount_available(g_mounts[existing].mount)) {
            log_errorf("vfs", "Mount point already in use: %s", normalized);
            return false;
        }
        if (existing >= 0) {
            g_mounts[existing].used = false;
            g_mounts[existing].mount = NULL;
        }
    }
    if (find_mount_slot_available() < 0) {
        log_errorf("vfs", "No free mount slots for %s", normalized);
        return false;
    }

    for (uint32_t i = 0; i < (sizeof(g_drivers) / sizeof(g_drivers[0])); i++) {
        const fs_driver_t* d = &g_drivers[i];
        if (!d->probe || !d->mount) continue;

        if (d->probe(dev)) {
            log_infof("vfs", "Probe matched: %s on %s", d->name, dev->name ? dev->name : "(noname)");
            if (mount_with_driver_at(d, normalized, dev)) return true;
            log_errorf("vfs", "Mount failed for driver %s", d->name);
        }
    }

    log_error("vfs", "No supported filesystem detected");
    return false;
}

bool vfs_mount_root_dev(block_device_t* dev) {
    return vfs_mount_dev("/", dev);
}

bool vfs_remount_root_dev(block_device_t* dev) {
    int slot = -1;

    if (!dev) {
        return false;
    }

    for (uint32_t i = 0; i < (sizeof(g_drivers) / sizeof(g_drivers[0])); i++) {
        const fs_driver_t* d = &g_drivers[i];
        vfs_mount_t* m = NULL;

        if (!d->probe || !d->mount) {
            continue;
        }

        if (!d->probe(dev)) {
            continue;
        }

        log_infof("vfs", "Probe matched: %s on %s", d->name, dev->name ? dev->name : "(noname)");
        if (!d->mount(dev, &m) || !m || !m->root) {
            log_errorf("vfs", "Root remount failed for driver %s", d->name);
            continue;
        }

        memset(m->mount_path, 0, sizeof(m->mount_path));
        memcpy(m->mount_path, "/", 2u);
        m->root->mount = m;

        slot = find_mount_slot_exact("/");
        if (slot < 0) {
            slot = find_mount_slot_available();
            if (slot < 0) {
                log_error("vfs", "No free mount slot for root remount");
                return false;
            }
            g_mounts[slot].used = true;
        }

        g_mounts[slot].mount = m;
        log_okf("vfs", "Remounted %s on %s at / (%s)",
                m->fs_name ? m->fs_name : "(unknown)",
                m->dev && m->dev->name ? m->dev->name : "(pseudo)",
                m->readonly ? "ro" : "rw");
        return true;
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
    return vfs_mount_dev("/", dev);
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

static bool split_parent_child(const char* abs_path,
                               char* parent_out,
                               uint32_t parent_cap,
                               char* name_out,
                               uint32_t name_cap) {
    uint32_t len = 0;
    uint32_t end = 0;
    uint32_t slash = 0;
    uint32_t name_len = 0;

    if (!is_abs_path(abs_path) || !parent_out || !name_out || parent_cap < 2 || name_cap < 2) {
        return false;
    }

    len = (uint32_t)strlen(abs_path);
    if (len <= 1) {
        return false;
    }

    end = len;
    while (end > 1 && abs_path[end - 1] == '/') {
        end--;
    }
    if (end <= 1) {
        return false;
    }

    slash = end - 1u;
    while (slash > 0 && abs_path[slash] != '/') {
        slash--;
    }
    if (abs_path[slash] != '/') {
        return false;
    }

    name_len = end - slash - 1u;
    if (name_len == 0 || (name_len + 1u) > name_cap) {
        return false;
    }

    memcpy(name_out, abs_path + slash + 1u, name_len);
    name_out[name_len] = '\0';

    if (strcmp(name_out, ".") == 0 || strcmp(name_out, "..") == 0) {
        return false;
    }

    if (slash == 0) {
        parent_out[0] = '/';
        parent_out[1] = '\0';
        return true;
    }

    if ((slash + 1u) > parent_cap) {
        return false;
    }

    memcpy(parent_out, abs_path, slash);
    parent_out[slash] = '\0';
    return true;
}

static bool path_has_mount_prefix(const char* abs_path, const char* mount_path, uint32_t* out_len) {
    uint32_t mount_len = 0;

    if (!abs_path || !mount_path || mount_path[0] != '/') {
        return false;
    }

    mount_len = (uint32_t)strlen(mount_path);
    if (mount_len == 1u && mount_path[0] == '/') {
        if (abs_path[0] != '/') {
            return false;
        }
        if (out_len) {
            *out_len = 1u;
        }
        return true;
    }

    if (strncmp(abs_path, mount_path, mount_len) != 0) {
        return false;
    }

    if (abs_path[mount_len] != '\0' && abs_path[mount_len] != '/') {
        return false;
    }

    if (out_len) {
        *out_len = mount_len;
    }
    return true;
}

static vfs_mount_t* select_mount_for_path(const char* abs_path, const char** out_rel_path) {
    vfs_mount_t* best = NULL;
    uint32_t best_len = 0;

    if (!abs_path || abs_path[0] != '/') {
        return NULL;
    }

    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        uint32_t mount_len = 0;

        if (!g_mounts[i].used || !g_mounts[i].mount) {
            continue;
        }
        if (!mount_available(g_mounts[i].mount)) {
            continue;
        }

        if (!path_has_mount_prefix(abs_path, g_mounts[i].mount->mount_path, &mount_len)) {
            continue;
        }

        if (!best || mount_len > best_len) {
            best = g_mounts[i].mount;
            best_len = mount_len;
        }
    }

    if (!best) {
        return NULL;
    }

    if (out_rel_path) {
        if (best_len == 1u) {
            *out_rel_path = abs_path;
        } else if (abs_path[best_len] == '\0') {
            *out_rel_path = "/";
        } else {
            *out_rel_path = abs_path + best_len;
        }
    }

    return best;
}

static vnode_t* clone_mount_root(vfs_mount_t* mount) {
    vnode_t* root = NULL;

    if (!mount || !mount->root) {
        return NULL;
    }

    root = (vnode_t*)kmalloc(sizeof(vnode_t));
    if (!root) {
        return NULL;
    }

    *root = *mount->root;
    root->refcount = 1;
    return root;
}

static bool mount_direct_child_name(const char* dir_path,
                                    const vfs_mount_t* mount,
                                    char* out_name,
                                    uint32_t out_cap) {
    const char* rem = NULL;
    uint32_t n = 0;

    if (!dir_path || !mount || !out_name || out_cap < 2u) {
        return false;
    }

    if (strcmp(dir_path, mount->mount_path) == 0) {
        return false;
    }

    if (strcmp(dir_path, "/") == 0) {
        if (mount->mount_path[0] != '/' || mount->mount_path[1] == '\0') {
            return false;
        }
        rem = mount->mount_path + 1u;
    } else {
        uint32_t dir_len = (uint32_t)strlen(dir_path);
        if (strncmp(mount->mount_path, dir_path, dir_len) != 0) {
            return false;
        }
        if (mount->mount_path[dir_len] != '/') {
            return false;
        }
        rem = mount->mount_path + dir_len + 1u;
    }

    if (!rem || *rem == '\0') {
        return false;
    }

    while (rem[n] != '\0' && rem[n] != '/') {
        if ((n + 1u) >= out_cap) {
            return false;
        }
        out_name[n] = rem[n];
        n++;
    }

    if (n == 0u) {
        return false;
    }

    out_name[n] = '\0';
    return true;
}

bool vfs_readdir(const char* abs_path, vfs_readdir_cb cb, void* user) {
    char normalized[VFS_MAX_MOUNT_PATH];
    vnode_t* dir = NULL;
    char seen_mount_names[VFS_MAX_MOUNTS][256];
    uint32_t seen_count = 0;

    if (!cb || !normalize_mount_path(abs_path, normalized, sizeof(normalized))) {
        return false;
    }

    if (!vfs_resolve(normalized, &dir) || !dir) {
        return false;
    }

    if (dir->type != VNODE_DIR || !dir->ops || !dir->ops->readdir) {
        vfs_vnode_put(dir);
        return false;
    }

    if (!dir->ops->readdir(dir, cb, user)) {
        vfs_vnode_put(dir);
        return true;
    }

    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        char child_name[256];
        bool duplicate_mount = false;

        if (!g_mounts[i].used || !g_mounts[i].mount) {
            continue;
        }
        if (!mount_available(g_mounts[i].mount)) {
            continue;
        }

        if (!mount_direct_child_name(normalized, g_mounts[i].mount, child_name, sizeof(child_name))) {
            continue;
        }

        for (uint32_t j = 0; j < seen_count; j++) {
            if (strcmp(seen_mount_names[j], child_name) == 0) {
                duplicate_mount = true;
                break;
            }
        }
        if (duplicate_mount) {
            continue;
        }

        if (dir->ops->lookup) {
            vnode_t* existing = NULL;
            if (dir->ops->lookup(dir, child_name, &existing) && existing) {
                vfs_vnode_put(existing);
                continue;
            }
        }

        if (seen_count < VFS_MAX_MOUNTS) {
            memcpy(seen_mount_names[seen_count], child_name, strlen(child_name) + 1u);
            seen_count++;
        }

        if (!cb(child_name,
                g_mounts[i].mount->root ? g_mounts[i].mount->root->ino : 0u,
                user)) {
            vfs_vnode_put(dir);
            return true;
        }
    }

    vfs_vnode_put(dir);
    return true;
}

bool vfs_resolve(const char* abs_path, vnode_t** out) {
    vfs_mount_t* mount = NULL;
    const char* rel_path = NULL;

    if (!out) return false;
    *out = NULL;

    if (!is_abs_path(abs_path)) {
        return false;
    }

    mount = select_mount_for_path(abs_path, &rel_path);
    if (!mount || !mount->root) {
        return false;
    }

    if (strcmp(rel_path, "/") == 0) {
        vnode_t* root = clone_mount_root(mount);
        if (!root) return false;
        *out = root;
        return true;
    }

    vnode_t* cur = clone_mount_root(mount);
    if (!cur) return false;

    const char* p = rel_path;
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

bool vfs_create(const char* abs_path, uint32_t mode, vnode_t** out) {
    char parent_path[256];
    char name[256];
    vnode_t* parent = NULL;
    vnode_t* created = NULL;
    bool ok = false;

    if (out) {
        *out = NULL;
    }

    if (!split_parent_child(abs_path, parent_path, sizeof(parent_path), name, sizeof(name))) {
        return false;
    }

    if (!vfs_resolve(parent_path, &parent) || !parent) {
        return false;
    }

    if (parent->mount && parent->mount->readonly) {
        vfs_vnode_put(parent);
        return false;
    }

    if (parent->type != VNODE_DIR || !parent->ops || !parent->ops->create) {
        vfs_vnode_put(parent);
        return false;
    }

    ok = parent->ops->create(parent, name, mode, &created);
    vfs_vnode_put(parent);
    if (!ok) {
        if (created) {
            vfs_vnode_put(created);
        }
        return false;
    }

    if (!out && created) {
        vfs_vnode_put(created);
    } else if (out) {
        *out = created;
    }

    return true;
}

bool vfs_mkdir(const char* abs_path, uint32_t mode) {
    char parent_path[256];
    char name[256];
    vnode_t* parent = NULL;
    bool ok = false;

    if (!split_parent_child(abs_path, parent_path, sizeof(parent_path), name, sizeof(name))) {
        return false;
    }

    if (!vfs_resolve(parent_path, &parent) || !parent) {
        return false;
    }

    if (parent->mount && parent->mount->readonly) {
        vfs_vnode_put(parent);
        return false;
    }

    if (parent->type != VNODE_DIR || !parent->ops || !parent->ops->mkdir) {
        vfs_vnode_put(parent);
        return false;
    }

    ok = parent->ops->mkdir(parent, name, mode);
    vfs_vnode_put(parent);
    return ok;
}

bool vfs_unlink(const char* abs_path) {
    char parent_path[256];
    char name[256];
    vnode_t* parent = NULL;
    bool ok = false;

    if (!split_parent_child(abs_path, parent_path, sizeof(parent_path), name, sizeof(name))) {
        return false;
    }

    if (!vfs_resolve(parent_path, &parent) || !parent) {
        return false;
    }

    if (parent->mount && parent->mount->readonly) {
        vfs_vnode_put(parent);
        return false;
    }

    if (parent->type != VNODE_DIR || !parent->ops || !parent->ops->unlink) {
        vfs_vnode_put(parent);
        return false;
    }

    ok = parent->ops->unlink(parent, name);
    vfs_vnode_put(parent);
    return ok;
}

void vfs_vnode_get(vnode_t* vn) {
    if (!vn) {
        return;
    }
    if (vn->refcount == 0) {
        vn->refcount = 1;
    }
    vn->refcount++;
}

void vfs_vnode_put(vnode_t* vn) {
    if (!vn) return;
    if (vn->refcount > 1) {
        vn->refcount--;
        return;
    }
    if (vn->ops && vn->ops->release) {
        vn->ops->release(vn);
    }
    kfree(vn);
}
