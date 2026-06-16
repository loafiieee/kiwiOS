#include "fs/kifs/kifs.h"
#include "fs/kifs/kifs_disk.h"
#include "fs/bcache.h"
#include "core/log.h"
#include "memory/heap.h"
#include "libc/string.h"
#include "libc/crc32.h"

#define KIFS_FS_MAJOR 0
#define KIFS_FS_MINOR 1

// ---------------- in-memory state ----------------

typedef struct {
    block_device_t* dev;
    kifs_superblock_t sb; // selected/validated superblock

    // Derived layout
    uint32_t dev_blocks;     // total blocks on device (4 KiB units)
    uint32_t usable_blocks;  // sb.usable_blocks
    uint32_t data_start;
    uint32_t data_end;       // exclusive
    uint32_t journal_start;

    bool readonly;
    bool meta_crc;
} kifs_fs_t;

// ---------------- utilities ----------------

static inline uint32_t u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint64_t u64_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

static bool magic_eq(const char m[4], const char* s4) {
    return m[0] == s4[0] && m[1] == s4[1] && m[2] == s4[2] && m[3] == s4[3];
}

static uint32_t calc_block_crc32(uint8_t* block_4k) {
    kifs_shdr_t* h = (kifs_shdr_t*)block_4k;
    uint32_t saved = h->checksum;
    h->checksum = 0;
    uint32_t crc = crc32_ieee(block_4k, KIFS_BLOCK_SIZE);
    h->checksum = saved;
    return crc;
}

static bool validate_shdr(const uint8_t* block_4k,
                          const char* expected_magic,
                          uint16_t expected_type,
                          bool require_crc) {
    const kifs_shdr_t* h = (const kifs_shdr_t*)block_4k;

    if (!magic_eq(h->magic, expected_magic)) return false;
    if (h->version != KIFS_STRUCT_VERSION) return false;
    if (h->block_type != expected_type) return false;

    if (h->header_bytes < sizeof(kifs_shdr_t) || h->header_bytes > KIFS_BLOCK_SIZE) return false;
    if (h->payload_bytes > KIFS_BLOCK_SIZE) return false;
    if ((uint32_t)h->header_bytes + (uint32_t)h->payload_bytes > KIFS_BLOCK_SIZE) return false;

    if (require_crc) {
        uint32_t crc = calc_block_crc32((uint8_t*)block_4k);
        if (crc != h->checksum) return false;
    }

    return true;
}

static bool sb_layout_sane(const kifs_superblock_t* sb, uint32_t dev_blocks) {
    if (!sb) return false;
    if (sb->block_size != KIFS_BLOCK_SIZE) return false;
    if (sb->total_blocks == 0) return false;
    if (sb->total_blocks > dev_blocks) return false;

    if (sb->usable_blocks > sb->total_blocks) return false;

    // v0.1 expects journal at the end
    if (sb->journal_start != sb->usable_blocks) return false;
    if (sb->journal_start + sb->journal_blocks != sb->total_blocks) return false;

    // superblocks must be inside usable range
    if (0 >= sb->usable_blocks || 1 >= sb->usable_blocks) return false;

    // regions must be within usable range
    uint32_t u = sb->usable_blocks;

    uint32_t bb_end = sb->block_bitmap_start + sb->block_bitmap_blocks;
    uint32_t ib_end = sb->inode_bitmap_start + sb->inode_bitmap_blocks;
    uint32_t it_end = sb->inode_table_start + sb->inode_table_blocks;
    uint32_t d_end  = sb->data_start + sb->data_blocks;

    if (sb->block_bitmap_start < 2) return false;
    if (bb_end > u) return false;
    if (sb->inode_bitmap_start < bb_end) return false;
    if (ib_end > u) return false;
    if (sb->inode_table_start < ib_end) return false;
    if (it_end > u) return false;
    if (sb->data_start < it_end) return false;
    if (d_end != u) return false;

    if (sb->inode_count == 0) return false;
    if (sb->root_ino != 1) return false;
    if (sb->root_ino >= sb->inode_count) return false;

    // Inode table sizing should match inode_count.
    uint32_t expected_it_blocks = (sb->inode_count + 15u) / 16u;
    if (sb->inode_table_blocks != expected_it_blocks) return false;

    return true;
}

static bool read_block_raw(block_device_t* dev, uint32_t blkno, uint8_t out[4096]) {
    bcache_buf_t* b = bcache_get(dev, blkno);
    if (!b) return false;
    memcpy(out, bcache_data(b), 4096);
    bcache_put(b);
    return true;
}

static bool write_block_raw(block_device_t* dev, uint32_t blkno, const uint8_t in[4096]) {
    bcache_buf_t* b = bcache_get(dev, blkno);
    if (!b) return false;
    memcpy(bcache_data(b), in, 4096);
    bcache_mark_dirty(b);
    bcache_put(b);
    return true;
}

static bool validate_superblock_at(block_device_t* dev, uint32_t dev_blocks, uint32_t blkno, kifs_superblock_t* out_sb) {
    uint8_t buf[4096];
    if (!read_block_raw(dev, blkno, buf)) return false;

    if (!validate_shdr(buf, KIFS_MAGIC_SUPER, KIFS_BTYPE_SUPER, true)) return false;

    const kifs_superblock_t* sb = (const kifs_superblock_t*)buf;
    if (sb->sb_blockno != blkno) return false;
    if (!sb_layout_sane(sb, dev_blocks)) return false;

    if (out_sb) memcpy(out_sb, sb, sizeof(*out_sb));
    return true;
}

static bool pick_superblock(block_device_t* dev, uint32_t dev_blocks, kifs_superblock_t* out) {
    kifs_superblock_t a = {0}, b = {0};
    bool va = validate_superblock_at(dev, dev_blocks, 0, &a);
    bool vb = validate_superblock_at(dev, dev_blocks, 1, &b);

    if (!va && !vb) return false;

    if (va && !vb) { *out = a; return true; }
    if (vb && !va) { *out = b; return true; }

    // both valid: higher seq wins; tie => primary
    if (a.sb_seq > b.sb_seq) { *out = a; return true; }
    if (b.sb_seq > a.sb_seq) { *out = b; return true; }

    *out = a;
    return true;
}

// ---------------- inode reading & validation ----------------

static bool disk_block_in_data_region(const kifs_fs_t* fs, uint32_t blk, uint32_t count) {
    if (!fs) return false;
    if (count == 0) return false;

    uint64_t start = (uint64_t)blk;
    uint64_t end_excl = start + (uint64_t)count;

    if (start < fs->data_start) return false;
    if (end_excl > fs->data_end) return false;

    // Must not be in journal region
    if (start >= fs->journal_start) return false;
    if (end_excl > fs->journal_start) return false;

    return true;
}

static bool inode_validate(const kifs_fs_t* fs, const kifs_inode_t* ino) {
    if (!fs || !ino) return false;

    if (!magic_eq(ino->magic, KIFS_MAGIC_INODE)) return false;
    if (ino->version != 1) return false;

    if (!(ino->type == KIFS_INO_T_FILE || ino->type == KIFS_INO_T_DIR)) return false;

    if (ino->inline_extent_count > 8) return false;
    if (ino->extent_tree_height > 3) return false;

    // Validate inline extents sorted, non-overlapping, and in-range
    uint32_t prev_file_end = 0;
    for (uint16_t i = 0; i < ino->inline_extent_count; i++) {
        const kifs_extent_t* e = &ino->inline_extents[i];
        if (e->block_count == 0) return false;

        // sorted by file_block_start
        if (i > 0) {
            if (e->file_block_start < prev_file_end) return false;
        }
        prev_file_end = e->file_block_start + e->block_count;

        if (!disk_block_in_data_region(fs, e->disk_block_start, e->block_count)) return false;
    }

    if (ino->extent_tree_height == 0) {
        if (ino->extent_tree_root != 0) {
            // tolerate, but treat as invalid to avoid following garbage
            return false;
        }
    } else {
        if (ino->extent_tree_root == 0) return false;
        if (!disk_block_in_data_region(fs, ino->extent_tree_root, 1)) return false;
    }

    return true;
}

static bool inode_read(const kifs_fs_t* fs, uint32_t ino_num, kifs_inode_t* out) {
    if (!fs || !out) return false;
    if (ino_num == 0) return false;
    if (ino_num >= fs->sb.inode_count) return false;

    uint32_t idx = ino_num;
    uint32_t block = fs->sb.inode_table_start + (idx / 16u);
    uint32_t off = (idx % 16u) * 256u;

    bcache_buf_t* b = bcache_get(fs->dev, block);
    if (!b) return false;

    memcpy(out, ((uint8_t*)bcache_data(b)) + off, sizeof(kifs_inode_t));
    bcache_put(b);

    return inode_validate(fs, out);
}

// ---------------- extent mapping ----------------

static bool extent_covers(const kifs_extent_t* e, uint32_t file_blk) {
    if (!e) return false;
    return (file_blk >= e->file_block_start) &&
           (file_blk < e->file_block_start + e->block_count);
}

static bool map_inline(const kifs_inode_t* ino, uint32_t file_blk, uint32_t* out_disk_blk) {
    for (uint16_t i = 0; i < ino->inline_extent_count; i++) {
        const kifs_extent_t* e = &ino->inline_extents[i];
        if (extent_covers(e, file_blk)) {
            uint32_t delta = file_blk - e->file_block_start;
            *out_disk_blk = e->disk_block_start + delta;
            return true;
        }
    }
    return false;
}

static bool extnode_validate(const kifs_fs_t* fs, const uint8_t* buf, uint32_t blkno) {
    (void)blkno;
    if (!validate_shdr(buf, KIFS_MAGIC_EXT, KIFS_BTYPE_EXT, fs->meta_crc)) return false;

    const kifs_ext_node_hdr_t* nh = (const kifs_ext_node_hdr_t*)buf;

    if (!(nh->node_type == 0 || nh->node_type == 1)) return false;
    if (nh->height > 3) return false;

    // header_bytes must include ext header fields
    if (nh->h.header_bytes < sizeof(kifs_ext_node_hdr_t) || nh->h.header_bytes > 4096) return false;

    // entry_count must fit payload
    uint32_t entry_size = (nh->node_type == 1) ? sizeof(kifs_ext_internal_ent_t) : sizeof(kifs_extent_t);
    uint32_t need = (uint32_t)nh->entry_count * entry_size;
    if (need > nh->h.payload_bytes) return false;

    // payload must start within block
    if ((uint32_t)nh->h.header_bytes + (uint32_t)nh->h.payload_bytes > 4096) return false;

    // Extra sanity: internal nodes must have height>0, leaf height==0
    if (nh->node_type == 0 && nh->height != 0) return false;
    if (nh->node_type == 1 && nh->height == 0) return false;

    // Validate sorted keys/extents (lightweight)
    const uint8_t* payload = buf + nh->h.header_bytes;

    if (nh->node_type == 1) {
        // internal: keys ascending
        uint32_t prev = 0;
        for (uint16_t i = 0; i < nh->entry_count; i++) {
            const kifs_ext_internal_ent_t* e = (const kifs_ext_internal_ent_t*)(payload + (uint32_t)i * entry_size);
            if (i > 0 && e->first_file_block < prev) return false;
            prev = e->first_file_block;
            if (!disk_block_in_data_region(fs, e->child_blockno, 1)) return false;
        }
    } else {
        // leaf: extents sorted, non-overlapping, in-range
        uint32_t prev_end = 0;
        for (uint16_t i = 0; i < nh->entry_count; i++) {
            const kifs_extent_t* e = (const kifs_extent_t*)(payload + (uint32_t)i * entry_size);
            if (e->block_count == 0) return false;
            if (i > 0) {
                if (e->file_block_start < prev_end) return false;
            }
            prev_end = e->file_block_start + e->block_count;
            if (!disk_block_in_data_region(fs, e->disk_block_start, e->block_count)) return false;
        }
    }

    return true;
}

static bool extent_tree_lookup(const kifs_fs_t* fs, const kifs_inode_t* ino, uint32_t file_blk, uint32_t* out_disk_blk) {
    if (!fs || !ino || !out_disk_blk) return false;
    if (ino->extent_tree_height == 0) return false;

    uint32_t cur_blk = ino->extent_tree_root;
    uint16_t expected_height = ino->extent_tree_height;

    for (uint16_t depth = expected_height; ; ) {
        bcache_buf_t* b = bcache_get(fs->dev, cur_blk);
        if (!b) return false;

        uint8_t buf[4096];
        memcpy(buf, bcache_data(b), 4096);
        bcache_put(b);

        if (!extnode_validate(fs, buf, cur_blk)) {
            return false; // treat subtree absent
        }

        const kifs_ext_node_hdr_t* nh = (const kifs_ext_node_hdr_t*)buf;

        if (nh->height != depth) {
            return false;
        }

        const uint8_t* payload = buf + nh->h.header_bytes;

        if (nh->node_type == 1) {
            // internal
            if (depth == 0) return false;

            const kifs_ext_internal_ent_t* best = NULL;
            for (uint16_t i = 0; i < nh->entry_count; i++) {
                const kifs_ext_internal_ent_t* e = (const kifs_ext_internal_ent_t*)(payload + (uint32_t)i * sizeof(kifs_ext_internal_ent_t));
                if (e->first_file_block <= file_blk) {
                    best = e;
                } else {
                    break;
                }
            }

            if (!best) return false;

            cur_blk = best->child_blockno;
            depth--;
            continue;
        }

        // leaf
        if (depth != 0) return false;

        for (uint16_t i = 0; i < nh->entry_count; i++) {
            const kifs_extent_t* e = (const kifs_extent_t*)(payload + (uint32_t)i * sizeof(kifs_extent_t));
            if (extent_covers(e, file_blk)) {
                uint32_t delta = file_blk - e->file_block_start;
                *out_disk_blk = e->disk_block_start + delta;
                return true;
            }
            if (e->file_block_start > file_blk) break;
        }

        return false;
    }
}

static bool map_file_block(const kifs_fs_t* fs, const kifs_inode_t* ino, uint32_t file_blk, uint32_t* out_disk_blk) {
    if (map_inline(ino, file_blk, out_disk_blk)) return true;
    if (extent_tree_lookup(fs, ino, file_blk, out_disk_blk)) return true;
    return false; // sparse
}

// ---------------- directory parsing ----------------

typedef struct {
    const char* want;
    uint32_t found_ino;
    bool found;
} dir_lookup_ctx_t;

static bool dir_lookup_cb(const char* name, uint32_t ino, void* user) {
    dir_lookup_ctx_t* ctx = (dir_lookup_ctx_t*)user;
    if (!ctx || !name) return true;
    if (strcmp(name, ctx->want) == 0) {
        ctx->found = true;
        ctx->found_ino = ino;
        return false; // stop
    }
    return true;
}

static bool parse_dir_block(const kifs_fs_t* fs, const uint8_t* blk, vfs_readdir_cb cb, void* user) {
    // Validate header
    if (!validate_shdr(blk, KIFS_MAGIC_DIR, KIFS_BTYPE_DIR, fs->meta_crc)) {
        return true; // treat absent
    }

    const kifs_shdr_t* h = (const kifs_shdr_t*)blk;
    if (h->header_bytes < (sizeof(kifs_shdr_t) + 4u)) return true;

    uint32_t used = h->payload_bytes;
    uint32_t off = h->header_bytes;
    uint32_t end = h->header_bytes + used;

    while (off + 8u <= end) {
        const uint8_t* p = blk + off;

        uint32_t ino = *(const uint32_t*)(p + 0);
        uint16_t reclen = *(const uint16_t*)(p + 4);
        uint8_t namelen = *(const uint8_t*)(p + 6);
        // uint8_t ftype = *(const uint8_t*)(p + 7);

        // namelen is u8 on disk; cannot exceed 255. Validate structural bounds instead.
        if (reclen < (uint16_t)(8u + namelen)) break;
        if ((reclen & 7u) != 0) break;
        if (off + reclen > end) break;

        if (ino != 0) {
            char namebuf[256];
            memcpy(namebuf, p + 8, namelen);
            namebuf[namelen] = 0;

            if (cb) {
                if (!cb(namebuf, ino, user)) return false;
            }
        }

        off += reclen;
    }

    return true;
}

static bool dir_iterate(const kifs_fs_t* fs, const kifs_inode_t* dir_ino, vfs_readdir_cb cb, void* user) {
    uint64_t size = dir_ino->size_bytes;
    uint32_t nblocks = (uint32_t)((size + (KIFS_BLOCK_SIZE - 1)) / KIFS_BLOCK_SIZE);

    for (uint32_t file_blk = 0; file_blk < nblocks; file_blk++) {
        uint32_t disk_blk = 0;
        if (!map_file_block(fs, dir_ino, file_blk, &disk_blk)) {
            continue; // sparse dir block => empty
        }

        bcache_buf_t* b = bcache_get(fs->dev, disk_blk);
        if (!b) continue;

        uint8_t tmp[4096];
        memcpy(tmp, bcache_data(b), 4096);
        bcache_put(b);

        if (!parse_dir_block(fs, tmp, cb, user)) {
            return false;
        }
    }

    return true;
}

// ---------------- VFS vnode ops ----------------

static bool kifs_vnode_stat(vnode_t* vn, vfs_stat_t* out) {
    if (!vn || !out || !vn->mount || !vn->mount->fs_private) return false;
    kifs_fs_t* fs = (kifs_fs_t*)vn->mount->fs_private;

    kifs_inode_t ino = {0};
    if (!inode_read(fs, vn->ino, &ino)) return false;

    out->ino = vn->ino;
    out->size = ino.size_bytes;
    out->mtime = ino.mtime;
    out->ctime = ino.ctime;
    out->mode = ino.mode;
    out->link_count = ino.link_count;
    out->type = (ino.type == KIFS_INO_T_DIR) ? VNODE_DIR : VNODE_FILE;
    return true;
}

static bool kifs_vnode_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    if (!dir || !name || !out) return false;
    *out = NULL;

    if (!dir->mount || !dir->mount->fs_private) return false;
    kifs_fs_t* fs = (kifs_fs_t*)dir->mount->fs_private;

    kifs_inode_t din = {0};
    if (!inode_read(fs, dir->ino, &din)) return false;
    if (din.type != KIFS_INO_T_DIR) return false;

    dir_lookup_ctx_t ctx = { .want = name, .found = false, .found_ino = 0 };
    dir_iterate(fs, &din, dir_lookup_cb, &ctx);
    if (!ctx.found) return false;

    vnode_t* vn = (vnode_t*)kmalloc(sizeof(vnode_t));
    if (!vn) return false;
    memset(vn, 0, sizeof(*vn));
    vn->mount = dir->mount;
    vn->ino = ctx.found_ino;

    // Fill basic type/size
    kifs_inode_t fin = {0};
    if (!inode_read(fs, vn->ino, &fin)) {
        kfree(vn);
        return false;
    }

    vn->type = (fin.type == KIFS_INO_T_DIR) ? VNODE_DIR : VNODE_FILE;
    vn->size = fin.size_bytes;
    vn->ops = dir->ops;

    *out = vn;
    return true;
}

static bool kifs_vnode_readdir(vnode_t* dir, vfs_readdir_cb cb, void* user) {
    if (!dir || !dir->mount || !dir->mount->fs_private) return false;
    kifs_fs_t* fs = (kifs_fs_t*)dir->mount->fs_private;

    kifs_inode_t din = {0};
    if (!inode_read(fs, dir->ino, &din)) return false;
    if (din.type != KIFS_INO_T_DIR) return false;

    return dir_iterate(fs, &din, cb, user);
}

static int64_t kifs_vnode_read(vnode_t* vn, uint64_t offset, void* buf, uint64_t len) {
    if (!vn || !buf || len == 0) return 0;
    if (!vn->mount || !vn->mount->fs_private) return -1;
    kifs_fs_t* fs = (kifs_fs_t*)vn->mount->fs_private;

    kifs_inode_t ino = {0};
    if (!inode_read(fs, vn->ino, &ino)) return -1;
    if (ino.type != KIFS_INO_T_FILE && ino.type != KIFS_INO_T_DIR) return -1;

    uint64_t size = ino.size_bytes;
    if (offset >= size) return 0;

    uint64_t to_read = u64_min(len, size - offset);
    uint8_t* out = (uint8_t*)buf;
    uint64_t done = 0;

    while (done < to_read) {
        uint64_t cur = offset + done;
        uint32_t file_blk = (uint32_t)(cur / KIFS_BLOCK_SIZE);
        uint32_t in_blk = (uint32_t)(cur % KIFS_BLOCK_SIZE);
        uint64_t chunk = u64_min(to_read - done, (uint64_t)(KIFS_BLOCK_SIZE - in_blk));

        uint32_t disk_blk = 0;
        bool present = map_file_block(fs, &ino, file_blk, &disk_blk);

        if (!present) {
            // sparse
            memset(out + done, 0, (size_t)chunk);
            done += chunk;
            continue;
        }

        bcache_buf_t* b = bcache_get(fs->dev, disk_blk);
        if (!b) return -1;

        memcpy(out + done, ((uint8_t*)bcache_data(b)) + in_blk, (size_t)chunk);
        bcache_put(b);

        done += chunk;
    }

    return (int64_t)done;
}

static const vnode_ops_t g_kifs_vops = {
    .lookup = kifs_vnode_lookup,
    .readdir = kifs_vnode_readdir,
    .read = kifs_vnode_read,
    .stat = kifs_vnode_stat,
};

// ---------------- public driver API ----------------

bool kifs_probe(block_device_t* dev) {
    if (!dev) return false;
    if (dev->sector_size != 512) return false;
    if (dev->total_sectors == 0) return false;

    uint32_t dev_blocks = (uint32_t)(dev->total_sectors / (BCACHE_SECTORS_PER_BLOCK));
    if (dev_blocks < 8) return false;

    kifs_superblock_t sb;
    return pick_superblock(dev, dev_blocks, &sb);
}

bool kifs_mount(block_device_t* dev, vfs_mount_t** out_mount) {
    if (!out_mount) return false;
    *out_mount = NULL;

    if (!dev) return false;
    if (dev->sector_size != 512) return false;
    if (dev->total_sectors == 0) return false;

    uint32_t dev_blocks = (uint32_t)(dev->total_sectors / (BCACHE_SECTORS_PER_BLOCK));
    if (dev_blocks < 8) return false;

    kifs_superblock_t sb;
    if (!pick_superblock(dev, dev_blocks, &sb)) return false;

    // Feature checks
    const uint32_t known_incompat = KIFS_FEAT_INCOMPAT_META_CRC | KIFS_FEAT_INCOMPAT_ORPHAN_FILE;
    if (sb.features_incompat & ~known_incompat) {
        log_errorf("kifs", "Unknown incompatible features: %x", (unsigned)(sb.features_incompat & ~known_incompat));
        return false;
    }

    bool readonly = false;
    if (sb.features_ro_compat != 0) {
        // no known ro-compat bits yet
        readonly = true;
    }

    kifs_fs_t* fs = (kifs_fs_t*)kmalloc(sizeof(kifs_fs_t));
    if (!fs) return false;
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->sb = sb;
    fs->dev_blocks = dev_blocks;
    fs->usable_blocks = sb.usable_blocks;
    fs->data_start = sb.data_start;
    fs->data_end = sb.data_start + sb.data_blocks;
    fs->journal_start = sb.journal_start;
    fs->readonly = readonly;
    fs->meta_crc = (sb.features_incompat & KIFS_FEAT_INCOMPAT_META_CRC) != 0;

    vnode_t* root = (vnode_t*)kmalloc(sizeof(vnode_t));
    vfs_mount_t* m = (vfs_mount_t*)kmalloc(sizeof(vfs_mount_t));
    if (!root || !m) {
        if (root) kfree(root);
        if (m) kfree(m);
        kfree(fs);
        return false;
    }

    memset(root, 0, sizeof(*root));
    memset(m, 0, sizeof(*m));

    root->mount = m;
    root->ino = sb.root_ino;
    root->ops = &g_kifs_vops;

    kifs_inode_t rino;
    if (!inode_read(fs, sb.root_ino, &rino) || rino.type != KIFS_INO_T_DIR) {
        kfree(root);
        kfree(m);
        kfree(fs);
        return false;
    }

    root->type = VNODE_DIR;
    root->size = rino.size_bytes;

    m->fs_name = "kifs";
    m->dev = dev;
    m->root = root;
    m->readonly = readonly;
    m->fs_private = fs;

    // Mark dirty on mount (best-effort).
    // For now, only do this if mounted read-write.
    if (!readonly) {
        // bump seq, set dirty, write both copies
        kifs_superblock_t nsb = fs->sb;
        nsb.sb_seq++;
        nsb.dirty = 1;
        nsb.mount_count++;

        // primary
        nsb.sb_blockno = 0;
        nsb.h.checksum = 0;
        nsb.h.checksum = crc32_ieee(&nsb, KIFS_BLOCK_SIZE);
        write_block_raw(dev, 0, (const uint8_t*)&nsb);

        // backup
        nsb.sb_blockno = 1;
        nsb.h.checksum = 0;
        nsb.h.checksum = crc32_ieee(&nsb, KIFS_BLOCK_SIZE);
        write_block_raw(dev, 1, (const uint8_t*)&nsb);

        bcache_sync_dev(dev);

        fs->sb = nsb;
    }

    *out_mount = m;
    return true;
}

// ---------------- mkfs (simple) ----------------

// Bitmap blocks store bits in payload: bytes = 4096 - sizeof(kifs_shdr_t)
#define KIFS_BITMAP_PAYLOAD_BYTES (KIFS_BLOCK_SIZE - (uint32_t)sizeof(kifs_shdr_t))
#define KIFS_BITMAP_BITS_PER_BLOCK (KIFS_BITMAP_PAYLOAD_BYTES * 8u)

static void bitmap_set(uint8_t* bm_payload, uint32_t bit) {
    bm_payload[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static __attribute__((unused)) void bitmap_clear(uint8_t* bm_payload, uint32_t bit) {
    bm_payload[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
}

static bool bitmap_get(const uint8_t* bm_payload, uint32_t bit) {
    return (bm_payload[bit / 8u] & (uint8_t)(1u << (bit % 8u))) != 0;
}

bool kifs_debug_get_bitmap_bits(const vfs_mount_t* m, bool inode_bitmap, uint32_t start_bit, uint32_t nbits, uint8_t* out_bits) {
    if (!m || !out_bits || nbits == 0) return false;
    if (!m->fs_private) return false;
    if (!m->fs_name || strcmp(m->fs_name, "kifs") != 0) return false;

    const kifs_fs_t* fs = (const kifs_fs_t*)m->fs_private;

    uint32_t bm_start = inode_bitmap ? fs->sb.inode_bitmap_start : fs->sb.block_bitmap_start;
    uint32_t bm_blocks = inode_bitmap ? fs->sb.inode_bitmap_blocks : fs->sb.block_bitmap_blocks;
    const char* magic = inode_bitmap ? KIFS_MAGIC_IMB : KIFS_MAGIC_BMB;
    uint16_t btype = inode_bitmap ? KIFS_BTYPE_IMB : KIFS_BTYPE_BMB;

    // Bounds: reject obviously-out-of-range requests.
    uint64_t total_bits = (uint64_t)bm_blocks * (uint64_t)KIFS_BITMAP_BITS_PER_BLOCK;
    if ((uint64_t)start_bit >= total_bits) return false;
    if ((uint64_t)start_bit + (uint64_t)nbits > total_bits) return false;

    for (uint32_t i = 0; i < nbits; i++) {
        uint32_t bit_index = start_bit + i;
        uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
        uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
        if (bi >= bm_blocks) return false;

        uint32_t disk_blk = bm_start + bi;
        bcache_buf_t* b = bcache_get(fs->dev, disk_blk);
        if (!b) return false;
        const uint8_t* blk = (const uint8_t*)bcache_data(b);

        // Bitmap blocks are structured blocks with CRC in our v0.1 mkfs.
        bool ok = validate_shdr(blk, magic, btype, true);
        if (!ok) {
            bcache_put(b);
            // Treat as absent -> all zeros, consistent with "invalid structured block => absent".
            out_bits[i] = 0;
            continue;
        }

        const uint8_t* payload = blk + sizeof(kifs_shdr_t);
        out_bits[i] = bitmap_get(payload, bit) ? 1 : 0;
        bcache_put(b);
    }

    return true;
}

static bool bitmap_set_in_blocks(uint8_t* bm_mem, uint32_t bm_blocks, uint32_t bit_index) {
    uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
    if (bi >= bm_blocks) return false;
    uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
    uint8_t* payload = bm_mem + bi * 4096u + (uint32_t)sizeof(kifs_shdr_t);
    bitmap_set(payload, bit);
    return true;
}

static void init_bitmap_block(uint8_t* out_blk, const char* magic, uint16_t btype, bool with_crc) {
    memset(out_blk, 0, 4096);
    kifs_shdr_t* h = (kifs_shdr_t*)out_blk;
    memcpy(h->magic, magic, 4);
    h->version = KIFS_STRUCT_VERSION;
    h->block_type = btype;
    h->header_bytes = (uint16_t)sizeof(kifs_shdr_t);
    h->payload_bytes = (uint16_t)KIFS_BITMAP_PAYLOAD_BYTES;
    h->flags = 0;
    h->checksum = 0;

    if (with_crc) {
        h->checksum = crc32_ieee(out_blk, 4096);
    }
}

static void init_dir_block(uint8_t* out_blk, uint32_t parent_hint, bool with_crc) {
    memset(out_blk, 0, 4096);
    kifs_shdr_t* h = (kifs_shdr_t*)out_blk;
    memcpy(h->magic, KIFS_MAGIC_DIR, 4);
    h->version = KIFS_STRUCT_VERSION;
    h->block_type = KIFS_BTYPE_DIR;
    h->header_bytes = (uint16_t)(sizeof(kifs_shdr_t) + 4u);
    h->payload_bytes = (uint16_t)(4096u - (uint32_t)h->header_bytes); // used_bytes = full payload for reuse
    h->flags = 0;

    *(uint32_t*)(out_blk + sizeof(kifs_shdr_t)) = parent_hint;

    h->checksum = 0;
    if (with_crc) {
        h->checksum = crc32_ieee(out_blk, 4096);
    }
}

static uint16_t dir_rec_len(uint8_t name_len) {
    uint32_t n = 8u + (uint32_t)name_len;
    return (uint16_t)((n + 7u) & ~7u);
}

static void dir_write_ent(uint8_t* blk, uint32_t off, uint32_t ino, const char* name, uint8_t ftype, uint16_t rec_len) {
    uint8_t* p = blk + off;
    uint8_t namelen = (uint8_t)strlen(name);
    *(uint32_t*)(p + 0) = ino;
    *(uint16_t*)(p + 4) = rec_len;
    *(uint8_t*)(p + 6) = namelen;
    *(uint8_t*)(p + 7) = ftype;
    memcpy(p + 8, name, namelen);
    // remaining bytes are already zero
}

static void inode_zero(kifs_inode_t* i) {
    memset(i, 0, sizeof(*i));
    memcpy(i->magic, KIFS_MAGIC_INODE, 4);
    i->version = 1;
}

bool kifs_mkfs(block_device_t* dev, uint32_t inode_count) {
    if (!dev) return false;
    if (dev->sector_size != 512) return false;
    if (dev->total_sectors == 0) {
        log_error("kifs", "mkfs: device size unknown");
        return false;
    }

    uint32_t N = (uint32_t)(dev->total_sectors / BCACHE_SECTORS_PER_BLOCK);
    if (N < 64) {
        log_errorf("kifs", "mkfs: device too small (%u blocks)", N);
        return false;
    }

    uint32_t journal_blocks = N / 100u;
    uint32_t usable_blocks = N - journal_blocks;

    if (inode_count == 0) inode_count = 1024;
    if (inode_count < 16) inode_count = 16;

    // Compute layout; shrink inode_count if needed.
    while (1) {
        uint32_t bb_blocks = (usable_blocks + (KIFS_BITMAP_BITS_PER_BLOCK - 1u)) / KIFS_BITMAP_BITS_PER_BLOCK;
        uint32_t ib_blocks = (inode_count + (KIFS_BITMAP_BITS_PER_BLOCK - 1u)) / KIFS_BITMAP_BITS_PER_BLOCK;
        uint32_t it_blocks = (inode_count + 15u) / 16u;

        uint32_t data_start = 2u + bb_blocks + ib_blocks + it_blocks;
        if (data_start + 4u < usable_blocks) {
            // enough room
            // Fill superblock
            kifs_superblock_t sb;
            memset(&sb, 0, sizeof(sb));
            memcpy(sb.h.magic, KIFS_MAGIC_SUPER, 4);
            sb.h.version = KIFS_STRUCT_VERSION;
            sb.h.block_type = KIFS_BTYPE_SUPER;
            sb.h.header_bytes = (uint16_t)sizeof(kifs_shdr_t);
            sb.h.payload_bytes = (uint16_t)(4096u - (uint32_t)sizeof(kifs_shdr_t));
            sb.h.flags = 0;

            sb.fs_major = KIFS_FS_MAJOR;
            sb.fs_minor = KIFS_FS_MINOR;
            sb.block_size = KIFS_BLOCK_SIZE;

            sb.total_blocks = N;
            sb.usable_blocks = usable_blocks;

            sb.journal_start = usable_blocks;
            sb.journal_blocks = journal_blocks;

            sb.block_bitmap_start = 2u;
            sb.block_bitmap_blocks = bb_blocks;

            sb.inode_bitmap_start = sb.block_bitmap_start + bb_blocks;
            sb.inode_bitmap_blocks = ib_blocks;

            sb.inode_table_start = sb.inode_bitmap_start + ib_blocks;
            sb.inode_table_blocks = it_blocks;

            sb.data_start = data_start;
            sb.data_blocks = usable_blocks - data_start;

            sb.inode_count = inode_count;
            sb.root_ino = 1;
            sb.orphan_ino = 2;

            sb.features_compat = 0;
            sb.features_ro_compat = 0;
            sb.features_incompat = KIFS_FEAT_INCOMPAT_META_CRC | KIFS_FEAT_INCOMPAT_ORPHAN_FILE;

            sb.dirty = 0;
            sb.sb_seq = 1;
            sb.mount_count = 0;
            sb.last_mount_time = 0;

            // Allocate initial data blocks
            uint32_t root_dir_blk = data_start;
            uint32_t hello_blk = data_start + 1;

            // Build bitmaps in memory
            // --- block bitmap ---
            uint32_t bb_total_bytes = bb_blocks * 4096u;
            uint8_t* bb_mem = (uint8_t*)kmalloc(bb_total_bytes);
            if (!bb_mem) return false;
            memset(bb_mem, 0, bb_total_bytes);

            // Each bitmap block has header; bits start after header
            for (uint32_t i = 0; i < bb_blocks; i++) {
                init_bitmap_block(bb_mem + i * 4096u, KIFS_MAGIC_BMB, KIFS_BTYPE_BMB, true);
            }            // reserved metadata
            bitmap_set_in_blocks(bb_mem, bb_blocks, 0);
            bitmap_set_in_blocks(bb_mem, bb_blocks, 1);
            for (uint32_t i = 0; i < bb_blocks; i++) bitmap_set_in_blocks(bb_mem, bb_blocks, sb.block_bitmap_start + i);
            for (uint32_t i = 0; i < ib_blocks; i++) bitmap_set_in_blocks(bb_mem, bb_blocks, sb.inode_bitmap_start + i);
            for (uint32_t i = 0; i < it_blocks; i++) bitmap_set_in_blocks(bb_mem, bb_blocks, sb.inode_table_start + i);

            // allocated data
            bitmap_set_in_blocks(bb_mem, bb_blocks, root_dir_blk);
            bitmap_set_in_blocks(bb_mem, bb_blocks, hello_blk);

            // Recompute bitmap CRCs after bit changes
            for (uint32_t i = 0; i < bb_blocks; i++) {
                uint8_t* blk = bb_mem + i * 4096u;
                kifs_shdr_t* h = (kifs_shdr_t*)blk;
                h->checksum = 0;
                h->checksum = crc32_ieee(blk, 4096);
            }

            // --- inode bitmap ---
            uint32_t ib_total_bytes = ib_blocks * 4096u;
            uint8_t* ib_mem = (uint8_t*)kmalloc(ib_total_bytes);
            if (!ib_mem) { kfree(bb_mem); return false; }
            memset(ib_mem, 0, ib_total_bytes);

            for (uint32_t i = 0; i < ib_blocks; i++) {
                init_bitmap_block(ib_mem + i * 4096u, KIFS_MAGIC_IMB, KIFS_BTYPE_IMB, true);
            }            bitmap_set_in_blocks(ib_mem, ib_blocks, 0); // reserved
            bitmap_set_in_blocks(ib_mem, ib_blocks, 1); // root
            bitmap_set_in_blocks(ib_mem, ib_blocks, 2); // orphan file
            bitmap_set_in_blocks(ib_mem, ib_blocks, 3); // hello.txt

            for (uint32_t i = 0; i < ib_blocks; i++) {
                uint8_t* blk = ib_mem + i * 4096u;
                kifs_shdr_t* h = (kifs_shdr_t*)blk;
                h->checksum = 0;
                h->checksum = crc32_ieee(blk, 4096);
            }

            // --- inode table ---
            uint32_t it_total_bytes = it_blocks * 4096u;
            uint8_t* it_mem = (uint8_t*)kmalloc(it_total_bytes);
            if (!it_mem) { kfree(bb_mem); kfree(ib_mem); return false; }
            memset(it_mem, 0, it_total_bytes);

            // Root inode (1)
            kifs_inode_t root;
            inode_zero(&root);
            root.type = KIFS_INO_T_DIR;
            root.mode = 0755;
            root.link_count = 2;
            root.size_bytes = 4096;
            root.mtime = 0;
            root.ctime = 0;
            root.inline_extent_count = 1;
            root.inline_extents[0].file_block_start = 0;
            root.inline_extents[0].disk_block_start = root_dir_blk;
            root.inline_extents[0].block_count = 1;

            // Orphan inode (2)
            kifs_inode_t orphan;
            inode_zero(&orphan);
            orphan.type = KIFS_INO_T_FILE;
            orphan.mode = 0;
            orphan.link_count = 1;
            orphan.size_bytes = 0;

            // hello inode (3)
            const char* hello_text = "Hello from KiFS v0.1 on KiwiOS!\n";
            uint32_t hello_len = (uint32_t)strlen(hello_text);

            kifs_inode_t hello;
            inode_zero(&hello);
            hello.type = KIFS_INO_T_FILE;
            hello.mode = 0644;
            hello.link_count = 1;
            hello.size_bytes = hello_len;
            hello.inline_extent_count = 1;
            hello.inline_extents[0].file_block_start = 0;
            hello.inline_extents[0].disk_block_start = hello_blk;
            hello.inline_extents[0].block_count = 1;

            // Write inodes into table memory
            memcpy(it_mem + (1u * 256u), &root, sizeof(root));
            memcpy(it_mem + (2u * 256u), &orphan, sizeof(orphan));
            memcpy(it_mem + (3u * 256u), &hello, sizeof(hello));

            // --- root directory block ---
            uint8_t dirblk[4096];
            init_dir_block(dirblk, 1, true);

            uint32_t doff = (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes;

            // '.'
            uint16_t r1 = dir_rec_len(1);
            dir_write_ent(dirblk, doff, 1, ".", 2, r1);
            doff += r1;

            // '..'
            uint16_t r2 = dir_rec_len(2);
            dir_write_ent(dirblk, doff, 1, "..", 2, r2);
            doff += r2;

            // 'hello.txt' (last entry fills the block)
            const char* fname = "hello.txt";
            uint8_t nlen = (uint8_t)strlen(fname);
            uint16_t minr3 = dir_rec_len(nlen);
            uint16_t r3 = (uint16_t)((4096u - (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes) - doff);
            r3 = (uint16_t)(r3 & ~7u);
            if (r3 < minr3) r3 = minr3;
            dir_write_ent(dirblk, doff, 3, fname, 1, r3);

            // update dir checksum
            kifs_shdr_t* dh = (kifs_shdr_t*)dirblk;
            dh->checksum = 0;
            dh->checksum = crc32_ieee(dirblk, 4096);

            // --- hello data block ---
            uint8_t helloblk[4096];
            memset(helloblk, 0, 4096);
            memcpy(helloblk, hello_text, hello_len);

            // --- superblock blocks ---
            // primary
            kifs_superblock_t s0 = sb;
            s0.sb_blockno = 0;
            s0.h.checksum = 0;
            s0.h.checksum = crc32_ieee(&s0, 4096);

            // backup
            kifs_superblock_t s1 = sb;
            s1.sb_blockno = 1;
            s1.h.checksum = 0;
            s1.h.checksum = crc32_ieee(&s1, 4096);

            // Write everything
            write_block_raw(dev, 0, (const uint8_t*)&s0);
            write_block_raw(dev, 1, (const uint8_t*)&s1);

            for (uint32_t i = 0; i < bb_blocks; i++) {
                write_block_raw(dev, sb.block_bitmap_start + i, bb_mem + i * 4096u);
            }
            for (uint32_t i = 0; i < ib_blocks; i++) {
                write_block_raw(dev, sb.inode_bitmap_start + i, ib_mem + i * 4096u);
            }
            for (uint32_t i = 0; i < it_blocks; i++) {
                write_block_raw(dev, sb.inode_table_start + i, it_mem + i * 4096u);
            }

            write_block_raw(dev, root_dir_blk, dirblk);
            write_block_raw(dev, hello_blk, helloblk);

            bcache_sync_dev(dev);

            kfree(bb_mem);
            kfree(ib_mem);
            kfree(it_mem);

            log_okf("kifs", "mkfs complete: blocks=%u usable=%u inodes=%u data_start=%u", N, usable_blocks, inode_count, data_start);
            return true;
        }

        // not enough room: shrink inode_count
        if (inode_count <= 16) {
            log_error("kifs", "mkfs: layout does not fit");
            return false;
        }
        inode_count /= 2;
    }
}
