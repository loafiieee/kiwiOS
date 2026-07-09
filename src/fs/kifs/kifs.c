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

static const vnode_ops_t g_kifs_vops;

#define KIFS_BITMAP_PAYLOAD_BYTES (KIFS_BLOCK_SIZE - (uint32_t)sizeof(kifs_shdr_t))
#define KIFS_BITMAP_BITS_PER_BLOCK (KIFS_BITMAP_PAYLOAD_BYTES * 8u)

static void init_dir_block(uint8_t* out_blk, uint32_t parent_hint, bool with_crc);
static uint16_t dir_rec_len(uint8_t name_len);
static void dir_write_ent(uint8_t* blk, uint32_t off, uint32_t ino, const char* name, uint8_t ftype, uint16_t rec_len);
static void inode_zero(kifs_inode_t* i);

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
    uint8_t* buf = (uint8_t*)kmalloc(KIFS_BLOCK_SIZE);
    bool ok = false;

    if (!buf) return false;
    if (!read_block_raw(dev, blkno, buf)) goto out;

    if (!validate_shdr(buf, KIFS_MAGIC_SUPER, KIFS_BTYPE_SUPER, true)) goto out;

    const kifs_superblock_t* sb = (const kifs_superblock_t*)buf;
    if (sb->sb_blockno != blkno) goto out;
    if (!sb_layout_sane(sb, dev_blocks)) goto out;

    if (out_sb) memcpy(out_sb, sb, sizeof(*out_sb));
    ok = true;

out:
    kfree(buf);
    return ok;
}

static bool pick_superblock(block_device_t* dev, uint32_t dev_blocks, kifs_superblock_t* out) {
    kifs_superblock_t* a = (kifs_superblock_t*)kmalloc(sizeof(*a));
    kifs_superblock_t* b = (kifs_superblock_t*)kmalloc(sizeof(*b));
    bool va = false;
    bool vb = false;
    bool ok = false;

    if (!a || !b) {
        if (a) kfree(a);
        if (b) kfree(b);
        return false;
    }

    memset(a, 0, sizeof(*a));
    memset(b, 0, sizeof(*b));
    va = validate_superblock_at(dev, dev_blocks, 0, a);
    vb = validate_superblock_at(dev, dev_blocks, 1, b);

    if (!va && !vb) goto out;

    if (va && !vb) {
        if (out) *out = *a;
        ok = true;
        goto out;
    }
    if (vb && !va) {
        if (out) *out = *b;
        ok = true;
        goto out;
    }

    // both valid: higher seq wins; tie => primary
    if (a->sb_seq > b->sb_seq) {
        if (out) *out = *a;
        ok = true;
        goto out;
    }
    if (b->sb_seq > a->sb_seq) {
        if (out) *out = *b;
        ok = true;
        goto out;
    }

    if (out) *out = *a;
    ok = true;

out:
    kfree(a);
    kfree(b);
    return ok;
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
    uint8_t* buf = (uint8_t*)kmalloc(KIFS_BLOCK_SIZE);
    if (!buf) return false;

    for (uint16_t depth = expected_height; ; ) {
        bcache_buf_t* b = bcache_get(fs->dev, cur_blk);
        if (!b) {
            kfree(buf);
            return false;
        }

        memcpy(buf, bcache_data(b), KIFS_BLOCK_SIZE);
        bcache_put(b);

        if (!extnode_validate(fs, buf, cur_blk)) {
            kfree(buf);
            return false; // treat subtree absent
        }

        const kifs_ext_node_hdr_t* nh = (const kifs_ext_node_hdr_t*)buf;

        if (nh->height != depth) {
            kfree(buf);
            return false;
        }

        const uint8_t* payload = buf + nh->h.header_bytes;

        if (nh->node_type == 1) {
            // internal
            if (depth == 0) {
                kfree(buf);
                return false;
            }

            const kifs_ext_internal_ent_t* best = NULL;
            for (uint16_t i = 0; i < nh->entry_count; i++) {
                const kifs_ext_internal_ent_t* e = (const kifs_ext_internal_ent_t*)(payload + (uint32_t)i * sizeof(kifs_ext_internal_ent_t));
                if (e->first_file_block <= file_blk) {
                    best = e;
                } else {
                    break;
                }
            }

            if (!best) {
                kfree(buf);
                return false;
            }

            cur_blk = best->child_blockno;
            depth--;
            continue;
        }

        // leaf
        if (depth != 0) {
            kfree(buf);
            return false;
        }

        for (uint16_t i = 0; i < nh->entry_count; i++) {
            const kifs_extent_t* e = (const kifs_extent_t*)(payload + (uint32_t)i * sizeof(kifs_extent_t));
            if (extent_covers(e, file_blk)) {
                uint32_t delta = file_blk - e->file_block_start;
                *out_disk_blk = e->disk_block_start + delta;
                kfree(buf);
                return true;
            }
            if (e->file_block_start > file_blk) break;
        }

        kfree(buf);
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
    uint8_t* tmp = (uint8_t*)kmalloc(KIFS_BLOCK_SIZE);

    if (!tmp) {
        return false;
    }

    for (uint32_t file_blk = 0; file_blk < nblocks; file_blk++) {
        uint32_t disk_blk = 0;
        if (!map_file_block(fs, dir_ino, file_blk, &disk_blk)) {
            continue; // sparse dir block => empty
        }

        bcache_buf_t* b = bcache_get(fs->dev, disk_blk);
        if (!b) continue;

        memcpy(tmp, bcache_data(b), KIFS_BLOCK_SIZE);
        bcache_put(b);

        if (!parse_dir_block(fs, tmp, cb, user)) {
            kfree(tmp);
            return false;
        }
    }

    kfree(tmp);
    return true;
}

typedef struct {
    bool found;
    uint32_t ino;
    uint32_t entry_off;
    uint16_t entry_reclen;
    bool prev_valid;
    uint32_t prev_off;
    uint16_t prev_reclen;
    uint16_t prev_min_reclen;
    bool slot_valid;
    uint32_t slot_off;
    uint16_t slot_reclen;
    uint16_t slot_min_reclen;
} kifs_dir_scan_t;

typedef struct {
    bool empty;
} dir_empty_ctx_t;

static bool dir_empty_cb(const char* name, uint32_t ino, void* user) {
    dir_empty_ctx_t* ctx = (dir_empty_ctx_t*)user;

    if (!ctx || ino == 0) {
        return true;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }

    ctx->empty = false;
    return false;
}

static void structured_rechecksum(uint8_t* blk, bool with_crc) {
    kifs_shdr_t* h = (kifs_shdr_t*)blk;

    h->checksum = 0;
    if (with_crc) {
        h->checksum = crc32_ieee(blk, KIFS_BLOCK_SIZE);
    }
}

static bool bitmap_payload_get(const uint8_t* payload, uint32_t bit) {
    return (payload[bit / 8u] & (uint8_t)(1u << (bit % 8u))) != 0;
}

static void bitmap_payload_set(uint8_t* payload, uint32_t bit) {
    payload[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static void bitmap_payload_clear(uint8_t* payload, uint32_t bit) {
    payload[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
}

static bool bitmap_access(const kifs_fs_t* fs,
                          bool inode_bitmap,
                          uint32_t bit_index,
                          bool* out_value,
                          bool update_value,
                          bool new_value) {
    uint32_t start = inode_bitmap ? fs->sb.inode_bitmap_start : fs->sb.block_bitmap_start;
    uint32_t blocks = inode_bitmap ? fs->sb.inode_bitmap_blocks : fs->sb.block_bitmap_blocks;
    const char* magic = inode_bitmap ? KIFS_MAGIC_IMB : KIFS_MAGIC_BMB;
    uint16_t btype = inode_bitmap ? KIFS_BTYPE_IMB : KIFS_BTYPE_BMB;
    uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
    bcache_buf_t* b = NULL;
    uint8_t* blk = NULL;
    uint8_t* payload = NULL;

    if (!fs || bi >= blocks) {
        return false;
    }

    b = bcache_get(fs->dev, start + bi);
    if (!b) {
        return false;
    }

    blk = (uint8_t*)bcache_data(b);
    if (!validate_shdr(blk, magic, btype, fs->meta_crc)) {
        bcache_put(b);
        return false;
    }

    payload = blk + sizeof(kifs_shdr_t);
    if (out_value) {
        *out_value = bitmap_payload_get(payload, bit);
    }

    if (update_value) {
        if (new_value) {
            bitmap_payload_set(payload, bit);
        } else {
            bitmap_payload_clear(payload, bit);
        }
        structured_rechecksum(blk, fs->meta_crc);
        bcache_mark_dirty(b);
    }

    bcache_put(b);
    return true;
}

static bool bitmap_alloc_range(const kifs_fs_t* fs,
                               bool inode_bitmap,
                               uint32_t start_bit,
                               uint32_t end_bit,
                               uint32_t* out_bit) {
    for (uint32_t bit = start_bit; bit < end_bit; bit++) {
        bool used = false;

        if (!bitmap_access(fs, inode_bitmap, bit, &used, false, false)) {
            return false;
        }
        if (used) {
            continue;
        }

        if (!bitmap_access(fs, inode_bitmap, bit, NULL, true, true)) {
            return false;
        }

        if (out_bit) {
            *out_bit = bit;
        }
        return true;
    }

    return false;
}

static bool inode_alloc(const kifs_fs_t* fs, uint32_t* out_ino) {
    return bitmap_alloc_range(fs, true, 1, fs->sb.inode_count, out_ino);
}

static bool data_block_alloc(const kifs_fs_t* fs, uint32_t* out_blk) {
    return bitmap_alloc_range(fs, false, fs->data_start, fs->data_end, out_blk);
}

static void inode_free(const kifs_fs_t* fs, uint32_t ino_num) {
    (void)bitmap_access(fs, true, ino_num, NULL, true, false);
}

static void data_block_free(const kifs_fs_t* fs, uint32_t blkno) {
    (void)bitmap_access(fs, false, blkno, NULL, true, false);
}

static bool inode_write(const kifs_fs_t* fs, uint32_t ino_num, const kifs_inode_t* in) {
    uint32_t block = 0;
    uint32_t off = 0;
    bcache_buf_t* b = NULL;
    uint8_t* dst = NULL;

    if (!fs || !in || ino_num == 0 || ino_num >= fs->sb.inode_count) {
        return false;
    }

    block = fs->sb.inode_table_start + (ino_num / 16u);
    off = (ino_num % 16u) * sizeof(kifs_inode_t);

    b = bcache_get(fs->dev, block);
    if (!b) {
        return false;
    }

    dst = ((uint8_t*)bcache_data(b)) + off;
    memcpy(dst, in, sizeof(*in));
    bcache_mark_dirty(b);
    bcache_put(b);
    return true;
}

static bool inode_clear(const kifs_fs_t* fs, uint32_t ino_num) {
    kifs_inode_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    return inode_write(fs, ino_num, &zeroed);
}

static bool vnode_from_inode(vfs_mount_t* mount, uint32_t ino_num, vnode_t** out) {
    kifs_fs_t* fs = NULL;
    kifs_inode_t ino;
    vnode_t* vn = NULL;

    if (!mount || !mount->fs_private || !out) {
        return false;
    }

    *out = NULL;
    fs = (kifs_fs_t*)mount->fs_private;
    if (!inode_read(fs, ino_num, &ino)) {
        return false;
    }

    vn = (vnode_t*)kmalloc(sizeof(vnode_t));
    if (!vn) {
        return false;
    }

    memset(vn, 0, sizeof(*vn));
    vn->mount = mount;
    vn->ino = ino_num;
    vn->type = (ino.type == KIFS_INO_T_DIR) ? VNODE_DIR : VNODE_FILE;
    vn->size = ino.size_bytes;
    vn->ops = &g_kifs_vops;

    *out = vn;
    return true;
}

static bool kifs_name_valid(const char* name) {
    uint32_t len = 0;

    if (!name || !name[0]) {
        return false;
    }

    while (name[len]) {
        if (name[len] == '/' || len >= 255u) {
            return false;
        }
        len++;
    }

    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool scan_dir_block_mutable(const uint8_t* blk, const char* name, uint16_t need_rec_len, kifs_dir_scan_t* out) {
    const kifs_shdr_t* h = (const kifs_shdr_t*)blk;
    uint32_t off = 0;
    uint32_t end = 0;
    bool prev_valid = false;
    uint32_t prev_off = 0;
    uint16_t prev_reclen = 0;
    uint16_t prev_min_reclen = 0;

    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    off = h->header_bytes;
    end = h->header_bytes + h->payload_bytes;

    while (off + 8u <= end) {
        const uint8_t* p = blk + off;
        uint32_t ino = *(const uint32_t*)(p + 0);
        uint16_t reclen = *(const uint16_t*)(p + 4);
        uint8_t namelen = *(const uint8_t*)(p + 6);
        uint16_t min_rec = dir_rec_len(namelen);

        if (reclen < min_rec || (reclen & 7u) != 0 || off + reclen > end) {
            break;
        }

        if (ino == 0) {
            if (!out->slot_valid &&
                need_rec_len != 0 &&
                reclen >= need_rec_len) {
                out->slot_valid = true;
                out->slot_off = off;
                out->slot_reclen = reclen;
                out->slot_min_reclen = 0;
            }
        } else {
            char cur_name[256];
            memcpy(cur_name, p + 8, namelen);
            cur_name[namelen] = '\0';

            if (!out->slot_valid &&
                need_rec_len != 0 &&
                reclen >= min_rec &&
                (uint16_t)(reclen - min_rec) >= need_rec_len) {
                out->slot_valid = true;
                out->slot_off = off;
                out->slot_reclen = reclen;
                out->slot_min_reclen = min_rec;
            }

            if (name && strcmp(cur_name, name) == 0) {
                out->found = true;
                out->ino = ino;
                out->entry_off = off;
                out->entry_reclen = reclen;
                out->prev_valid = prev_valid;
                out->prev_off = prev_off;
                out->prev_reclen = prev_reclen;
                out->prev_min_reclen = prev_min_reclen;
                return true;
            }

            prev_valid = true;
            prev_off = off;
            prev_reclen = reclen;
            prev_min_reclen = min_rec;
        }

        off += reclen;
    }

    return true;
}

static bool dir_append_entry_block(const kifs_fs_t* fs,
                                   uint32_t dir_ino_num,
                                   kifs_inode_t* dir_ino,
                                   const char* name,
                                   uint32_t child_ino,
                                   uint8_t ftype,
                                   uint16_t need_rec) {
    uint32_t file_blk = 0;
    uint32_t disk_blk = 0;
    bcache_buf_t* b = NULL;
    uint8_t* blk = NULL;
    kifs_extent_t* last = NULL;

    if (!fs || !dir_ino || dir_ino->type != KIFS_INO_T_DIR ||
        dir_ino->extent_tree_height != 0 || need_rec == 0u) {
        return false;
    }

    if (!data_block_alloc(fs, &disk_blk)) {
        return false;
    }

    b = bcache_get(fs->dev, disk_blk);
    if (!b) {
        data_block_free(fs, disk_blk);
        return false;
    }

    blk = (uint8_t*)bcache_data(b);
    init_dir_block(blk, dir_ino_num, fs->meta_crc);

    {
        kifs_shdr_t* h = (kifs_shdr_t*)blk;
        uint32_t off = h->header_bytes;
        uint16_t rec = (uint16_t)((KIFS_BLOCK_SIZE - off) & ~7u);

        if (rec < need_rec) {
            bcache_put(b);
            data_block_free(fs, disk_blk);
            return false;
        }

        dir_write_ent(blk, off, child_ino, name, ftype, rec);
        structured_rechecksum(blk, fs->meta_crc);
    }

    bcache_mark_dirty(b);
    bcache_put(b);

    file_blk = (uint32_t)((dir_ino->size_bytes + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);

    if (dir_ino->inline_extent_count > 0u) {
        last = &dir_ino->inline_extents[dir_ino->inline_extent_count - 1u];
        if (last->file_block_start + last->block_count == file_blk &&
            last->disk_block_start + last->block_count == disk_blk) {
            last->block_count++;
        } else if (dir_ino->inline_extent_count < 8u) {
            kifs_extent_t* e = &dir_ino->inline_extents[dir_ino->inline_extent_count++];
            e->file_block_start = file_blk;
            e->disk_block_start = disk_blk;
            e->block_count = 1u;
        } else {
            data_block_free(fs, disk_blk);
            return false;
        }
    } else {
        kifs_extent_t* e = &dir_ino->inline_extents[0];
        dir_ino->inline_extent_count = 1u;
        e->file_block_start = file_blk;
        e->disk_block_start = disk_blk;
        e->block_count = 1u;
    }

    dir_ino->size_bytes = ((uint64_t)file_blk + 1u) * KIFS_BLOCK_SIZE;
    dir_ino->mtime++;
    dir_ino->ctime++;

    if (!inode_write(fs, dir_ino_num, dir_ino)) {
        data_block_free(fs, disk_blk);
        return false;
    }

    return true;
}

static bool dir_add_entry(const kifs_fs_t* fs,
                          uint32_t dir_ino_num,
                          kifs_inode_t* dir_ino,
                          const char* name,
                          uint32_t child_ino,
                          uint8_t ftype) {
    uint64_t size = 0;
    uint32_t nblocks = 0;
    uint16_t need_rec = 0;

    if (!fs || !dir_ino || dir_ino->type != KIFS_INO_T_DIR || !kifs_name_valid(name)) {
        return false;
    }

    need_rec = dir_rec_len((uint8_t)strlen(name));
    size = dir_ino->size_bytes;
    nblocks = (uint32_t)((size + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);

    for (uint32_t file_blk = 0; file_blk < nblocks; file_blk++) {
        uint32_t disk_blk = 0;
        bcache_buf_t* b = NULL;
        uint8_t* blk = NULL;
        kifs_dir_scan_t scan;

        if (!map_file_block(fs, dir_ino, file_blk, &disk_blk)) {
            continue;
        }

        b = bcache_get(fs->dev, disk_blk);
        if (!b) {
            continue;
        }

        blk = (uint8_t*)bcache_data(b);
        if (!validate_shdr(blk, KIFS_MAGIC_DIR, KIFS_BTYPE_DIR, fs->meta_crc)) {
            bcache_put(b);
            continue;
        }

        if (!scan_dir_block_mutable(blk, name, need_rec, &scan)) {
            bcache_put(b);
            return false;
        }

        if (scan.found) {
            bcache_put(b);
            return false;
        }

        if (!scan.slot_valid) {
            bcache_put(b);
            continue;
        }

        if (scan.slot_min_reclen == 0u) {
            dir_write_ent(blk,
                          scan.slot_off,
                          child_ino,
                          name,
                          ftype,
                          scan.slot_reclen);
        } else {
            *(uint16_t*)(blk + scan.slot_off + 4u) = scan.slot_min_reclen;
            dir_write_ent(blk,
                          scan.slot_off + scan.slot_min_reclen,
                          child_ino,
                          name,
                          ftype,
                          (uint16_t)(scan.slot_reclen - scan.slot_min_reclen));
        }
        structured_rechecksum(blk, fs->meta_crc);
        bcache_mark_dirty(b);
        bcache_put(b);
        return true;
    }

    return dir_append_entry_block(fs, dir_ino_num, dir_ino, name, child_ino, ftype, need_rec);
}

static bool dir_remove_entry(const kifs_fs_t* fs, const kifs_inode_t* dir_ino, const char* name, uint32_t* out_ino) {
    uint64_t size = 0;
    uint32_t nblocks = 0;

    if (!fs || !dir_ino || dir_ino->type != KIFS_INO_T_DIR || !kifs_name_valid(name)) {
        return false;
    }

    size = dir_ino->size_bytes;
    nblocks = (uint32_t)((size + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);

    for (uint32_t file_blk = 0; file_blk < nblocks; file_blk++) {
        uint32_t disk_blk = 0;
        bcache_buf_t* b = NULL;
        uint8_t* blk = NULL;
        kifs_dir_scan_t scan;

        if (!map_file_block(fs, dir_ino, file_blk, &disk_blk)) {
            continue;
        }

        b = bcache_get(fs->dev, disk_blk);
        if (!b) {
            continue;
        }

        blk = (uint8_t*)bcache_data(b);
        if (!validate_shdr(blk, KIFS_MAGIC_DIR, KIFS_BTYPE_DIR, fs->meta_crc)) {
            bcache_put(b);
            continue;
        }

        if (!scan_dir_block_mutable(blk, name, 0, &scan)) {
            bcache_put(b);
            return false;
        }

        if (!scan.found) {
            bcache_put(b);
            continue;
        }

        if (!scan.prev_valid) {
            bcache_put(b);
            return false;
        }

        *(uint16_t*)(blk + scan.prev_off + 4u) = (uint16_t)(scan.prev_reclen + scan.entry_reclen);
        structured_rechecksum(blk, fs->meta_crc);
        bcache_mark_dirty(b);
        bcache_put(b);

        if (out_ino) {
            *out_ino = scan.ino;
        }
        return true;
    }

    return false;
}

static bool dir_is_empty(const kifs_fs_t* fs, const kifs_inode_t* dir_ino) {
    dir_empty_ctx_t ctx = { .empty = true };

    if (!fs || !dir_ino || dir_ino->type != KIFS_INO_T_DIR) {
        return false;
    }

    (void)dir_iterate(fs, dir_ino, dir_empty_cb, &ctx);
    return ctx.empty;
}

static bool allocate_extents(const kifs_fs_t* fs, uint32_t need_blocks, kifs_extent_t out_extents[8], uint16_t* out_count) {
    uint32_t remaining = need_blocks;
    uint32_t current_start = 0;
    uint32_t current_len = 0;
    uint32_t current_file_block = 0;
    uint16_t ext_count = 0;

    if (!fs || !out_extents || !out_count) {
        return false;
    }

    memset(out_extents, 0, sizeof(kifs_extent_t) * 8u);
    *out_count = 0;

    if (need_blocks == 0) {
        return true;
    }

    for (uint32_t blk = fs->data_start; blk < fs->data_end && remaining > 0; blk++) {
        bool used = false;

        if (!bitmap_access(fs, false, blk, &used, false, false)) {
            return false;
        }

        if (used) {
            if (current_len != 0) {
                if (ext_count >= 8u) {
                    return false;
                }
                out_extents[ext_count].file_block_start = current_file_block;
                out_extents[ext_count].disk_block_start = current_start;
                out_extents[ext_count].block_count = current_len;
                current_file_block += current_len;
                ext_count++;
                current_len = 0;
            }
            continue;
        }

        if (current_len == 0) {
            current_start = blk;
            current_len = 1;
        } else if (blk == current_start + current_len) {
            current_len++;
        } else {
            if (ext_count >= 8u) {
                return false;
            }
            out_extents[ext_count].file_block_start = current_file_block;
            out_extents[ext_count].disk_block_start = current_start;
            out_extents[ext_count].block_count = current_len;
            current_file_block += current_len;
            ext_count++;
            current_start = blk;
            current_len = 1;
        }

        remaining--;
    }

    if (current_len != 0) {
        if (ext_count >= 8u) {
            return false;
        }
        out_extents[ext_count].file_block_start = current_file_block;
        out_extents[ext_count].disk_block_start = current_start;
        out_extents[ext_count].block_count = current_len;
        ext_count++;
    }

    if (remaining != 0) {
        return false;
    }

    for (uint16_t i = 0; i < ext_count; i++) {
        for (uint32_t j = 0; j < out_extents[i].block_count; j++) {
            if (!bitmap_access(fs, false, out_extents[i].disk_block_start + j, NULL, true, true)) {
                for (uint16_t ri = 0; ri <= i; ri++) {
                    uint32_t rollback_count = (ri == i) ? j : out_extents[ri].block_count;
                    for (uint32_t rj = 0; rj < rollback_count; rj++) {
                        data_block_free(fs, out_extents[ri].disk_block_start + rj);
                    }
                }
                return false;
            }
        }
    }

    *out_count = ext_count;
    return true;
}

static void free_inode_data(const kifs_fs_t* fs, const kifs_inode_t* ino) {
    if (!fs || !ino || ino->extent_tree_height != 0) {
        return;
    }

    for (uint16_t i = 0; i < ino->inline_extent_count && i < 8u; i++) {
        const kifs_extent_t* e = &ino->inline_extents[i];
        for (uint32_t j = 0; j < e->block_count; j++) {
            data_block_free(fs, e->disk_block_start + j);
        }
    }
}

static void free_extent_array(const kifs_fs_t* fs, const kifs_extent_t* extents, uint16_t count) {
    if (!fs || !extents) {
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < extents[i].block_count; j++) {
            data_block_free(fs, extents[i].disk_block_start + j);
        }
    }
}

static bool write_extent_data(const kifs_fs_t* fs,
                              const uint8_t* data,
                              uint64_t len,
                              const kifs_extent_t* extents,
                              uint16_t extent_count) {
    uint64_t written = 0;

    for (uint16_t i = 0; i < extent_count; i++) {
        for (uint32_t j = 0; j < extents[i].block_count; j++) {
            bcache_buf_t* b = bcache_get(fs->dev, extents[i].disk_block_start + j);
            uint8_t* blk = NULL;
            uint64_t chunk = 0;

            if (!b) {
                return false;
            }

            blk = (uint8_t*)bcache_data(b);
            memset(blk, 0, KIFS_BLOCK_SIZE);

            chunk = len - written;
            if (chunk > KIFS_BLOCK_SIZE) {
                chunk = KIFS_BLOCK_SIZE;
            }
            if (chunk > 0 && data) {
                memcpy(blk, data + written, (size_t)chunk);
                written += chunk;
            }

            bcache_mark_dirty(b);
            bcache_put(b);
        }
    }

    return true;
}

static bool replace_file_contents(const kifs_fs_t* fs,
                                  vnode_t* vn,
                                  const kifs_inode_t* old_ino,
                                  const uint8_t* data,
                                  uint64_t len) {
    kifs_extent_t new_extents[8];
    uint16_t new_extent_count = 0;
    kifs_inode_t new_ino;
    uint32_t need_blocks = (uint32_t)((len + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);

    if (!fs || !vn || !old_ino || old_ino->type != KIFS_INO_T_FILE || old_ino->extent_tree_height != 0) {
        return false;
    }

    memset(new_extents, 0, sizeof(new_extents));
    if (!allocate_extents(fs, need_blocks, new_extents, &new_extent_count)) {
        return false;
    }

    if (!write_extent_data(fs, data, len, new_extents, new_extent_count)) {
        free_extent_array(fs, new_extents, new_extent_count);
        return false;
    }

    new_ino = *old_ino;
    new_ino.size_bytes = len;
    new_ino.inline_extent_count = new_extent_count;
    new_ino.extent_tree_height = 0;
    new_ino.extent_tree_root = 0;
    new_ino.mtime = old_ino->mtime + 1u;
    new_ino.ctime = old_ino->ctime + 1u;
    memset(new_ino.inline_extents, 0, sizeof(new_ino.inline_extents));
    for (uint16_t i = 0; i < new_extent_count; i++) {
        new_ino.inline_extents[i] = new_extents[i];
    }

    if (!inode_write(fs, vn->ino, &new_ino)) {
        free_extent_array(fs, new_extents, new_extent_count);
        return false;
    }

    free_inode_data(fs, old_ino);
    vn->size = len;
    return true;
}

static bool append_file_contents(const kifs_fs_t* fs,
                                 vnode_t* vn,
                                 const kifs_inode_t* old_ino,
                                 const uint8_t* data,
                                 uint64_t len) {
    kifs_extent_t new_extents[8];
    kifs_inode_t new_ino;
    uint64_t old_size = 0;
    uint64_t new_size = 0;
    uint64_t copied = 0;
    uint32_t first_new_file_blk = 0;
    uint32_t need_blocks = 0;
    uint16_t new_extent_count = 0;

    if (!fs || !vn || !old_ino || (!data && len != 0) ||
        old_ino->type != KIFS_INO_T_FILE || old_ino->extent_tree_height != 0 ||
        old_ino->inline_extent_count > 8u) {
        return false;
    }

    if (len == 0) {
        return true;
    }

    old_size = old_ino->size_bytes;
    if (old_size > UINT64_MAX - len) {
        return false;
    }
    new_size = old_size + len;

    new_ino = *old_ino;
    memset(new_extents, 0, sizeof(new_extents));

    if ((old_size % KIFS_BLOCK_SIZE) != 0u) {
        uint32_t file_blk = (uint32_t)(old_size / KIFS_BLOCK_SIZE);
        uint32_t in_blk = (uint32_t)(old_size % KIFS_BLOCK_SIZE);
        uint64_t chunk = u64_min(len, (uint64_t)(KIFS_BLOCK_SIZE - in_blk));
        uint32_t disk_blk = 0;
        bcache_buf_t* b = NULL;

        if (!map_file_block(fs, old_ino, file_blk, &disk_blk)) {
            return false;
        }

        b = bcache_get(fs->dev, disk_blk);
        if (!b) {
            return false;
        }
        memcpy(((uint8_t*)bcache_data(b)) + in_blk, data, (size_t)chunk);
        bcache_mark_dirty(b);
        bcache_put(b);
        copied = chunk;
    }

    first_new_file_blk = (uint32_t)((old_size + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);
    need_blocks = (uint32_t)(((len - copied) + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);

    if (need_blocks != 0u) {
        if (!allocate_extents(fs, need_blocks, new_extents, &new_extent_count)) {
            return false;
        }

        for (uint16_t i = 0; i < new_extent_count; i++) {
            new_extents[i].file_block_start += first_new_file_blk;
        }

        for (uint16_t i = 0; i < new_extent_count; i++) {
            kifs_extent_t e = new_extents[i];

            if (new_ino.inline_extent_count > 0u) {
                kifs_extent_t* last = &new_ino.inline_extents[new_ino.inline_extent_count - 1u];
                if (last->file_block_start + last->block_count == e.file_block_start &&
                    last->disk_block_start + last->block_count == e.disk_block_start) {
                    last->block_count += e.block_count;
                    continue;
                }
            }

            if (new_ino.inline_extent_count >= 8u) {
                free_extent_array(fs, new_extents, new_extent_count);
                return false;
            }
            new_ino.inline_extents[new_ino.inline_extent_count++] = e;
        }

        if (!write_extent_data(fs, data + copied, len - copied, new_extents, new_extent_count)) {
            free_extent_array(fs, new_extents, new_extent_count);
            return false;
        }
    }

    new_ino.size_bytes = new_size;
    new_ino.mtime = old_ino->mtime + 1u;
    new_ino.ctime = old_ino->ctime + 1u;
    if (!inode_write(fs, vn->ino, &new_ino)) {
        free_extent_array(fs, new_extents, new_extent_count);
        return false;
    }

    vn->size = new_size;
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

    return vnode_from_inode(dir->mount, ctx.found_ino, out);
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

static bool kifs_vnode_truncate(vnode_t* vn, uint64_t size) {
    kifs_fs_t* fs = NULL;
    kifs_inode_t ino;
    uint8_t* tmp = NULL;
    uint64_t keep = 0;
    bool ok = false;

    if (!vn || !vn->mount || !vn->mount->fs_private) {
        return false;
    }

    fs = (kifs_fs_t*)vn->mount->fs_private;
    if (fs->readonly || !inode_read(fs, vn->ino, &ino) || ino.type != KIFS_INO_T_FILE) {
        return false;
    }

    if (size == ino.size_bytes) {
        return true;
    }

    if (size != 0) {
        tmp = (uint8_t*)kmalloc((size_t)size);
        if (!tmp) {
            return false;
        }
        memset(tmp, 0, (size_t)size);

        keep = u64_min(size, ino.size_bytes);
        if (keep != 0 && kifs_vnode_read(vn, 0, tmp, keep) != (int64_t)keep) {
            kfree(tmp);
            return false;
        }
    }

    ok = replace_file_contents(fs, vn, &ino, tmp, size) && bcache_sync_dev(fs->dev);
    if (tmp) {
        kfree(tmp);
    }

    return ok;
}

static int64_t kifs_vnode_write(vnode_t* vn, uint64_t offset, const void* buf, uint64_t len) {
    kifs_fs_t* fs = NULL;
    kifs_inode_t ino;
    uint8_t* tmp = NULL;
    uint64_t new_size = 0;
    bool ok = false;

    if (!vn || (!buf && len != 0) || !vn->mount || !vn->mount->fs_private) {
        return -1;
    }

    fs = (kifs_fs_t*)vn->mount->fs_private;
    if (fs->readonly || !inode_read(fs, vn->ino, &ino) || ino.type != KIFS_INO_T_FILE) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    if (offset > UINT64_MAX - len) {
        return -1;
    }

    new_size = offset + len;
    if (new_size < ino.size_bytes) {
        new_size = ino.size_bytes;
    }

    if (offset == ino.size_bytes) {
        ok = append_file_contents(fs, vn, &ino, (const uint8_t*)buf, len) && bcache_sync_dev(fs->dev);
        return ok ? (int64_t)len : -1;
    }

    tmp = (uint8_t*)kmalloc((size_t)new_size);
    if (!tmp) {
        return -1;
    }
    memset(tmp, 0, (size_t)new_size);

    if (ino.size_bytes != 0 && kifs_vnode_read(vn, 0, tmp, ino.size_bytes) != (int64_t)ino.size_bytes) {
        kfree(tmp);
        return -1;
    }

    memcpy(tmp + offset, buf, (size_t)len);
    ok = replace_file_contents(fs, vn, &ino, tmp, new_size) && bcache_sync_dev(fs->dev);
    kfree(tmp);
    return ok ? (int64_t)len : -1;
}

static bool kifs_vnode_create(vnode_t* dir, const char* name, uint32_t mode, vnode_t** out) {
    kifs_fs_t* fs = NULL;
    kifs_inode_t parent;
    kifs_inode_t child;
    dir_lookup_ctx_t ctx;
    uint32_t ino_num = 0;

    if (out) {
        *out = NULL;
    }

    if (!dir || !dir->mount || !dir->mount->fs_private || !kifs_name_valid(name)) {
        return false;
    }

    fs = (kifs_fs_t*)dir->mount->fs_private;
    if (fs->readonly || !inode_read(fs, dir->ino, &parent) || parent.type != KIFS_INO_T_DIR) {
        return false;
    }

    ctx.want = name;
    ctx.found = false;
    ctx.found_ino = 0;
    (void)dir_iterate(fs, &parent, dir_lookup_cb, &ctx);
    if (ctx.found || !inode_alloc(fs, &ino_num)) {
        return false;
    }

    inode_zero(&child);
    child.type = KIFS_INO_T_FILE;
    child.mode = mode ? mode : 0644u;
    child.link_count = 1;
    child.size_bytes = 0;

    if (!inode_write(fs, ino_num, &child) ||
        !dir_add_entry(fs, dir->ino, &parent, name, ino_num, 1)) {
        (void)inode_clear(fs, ino_num);
        inode_free(fs, ino_num);
        return false;
    }

    if (!bcache_sync_dev(fs->dev)) {
        (void)dir_remove_entry(fs, &parent, name, NULL);
        (void)inode_clear(fs, ino_num);
        inode_free(fs, ino_num);
        return false;
    }

    return !out || vnode_from_inode(dir->mount, ino_num, out);
}

static bool kifs_vnode_mkdir(vnode_t* dir, const char* name, uint32_t mode) {
    kifs_fs_t* fs = NULL;
    kifs_inode_t parent;
    kifs_inode_t child;
    dir_lookup_ctx_t ctx;
    uint32_t ino_num = 0;
    uint32_t blkno = 0;
    uint8_t* dirblk = NULL;

    if (!dir || !dir->mount || !dir->mount->fs_private || !kifs_name_valid(name)) {
        return false;
    }

    fs = (kifs_fs_t*)dir->mount->fs_private;
    if (fs->readonly || !inode_read(fs, dir->ino, &parent) || parent.type != KIFS_INO_T_DIR) {
        return false;
    }

    ctx.want = name;
    ctx.found = false;
    ctx.found_ino = 0;
    (void)dir_iterate(fs, &parent, dir_lookup_cb, &ctx);
    if (ctx.found || !inode_alloc(fs, &ino_num) || !data_block_alloc(fs, &blkno)) {
        if (ino_num != 0) {
            inode_free(fs, ino_num);
        }
        return false;
    }

    dirblk = (uint8_t*)kmalloc(KIFS_BLOCK_SIZE);
    if (!dirblk) {
        data_block_free(fs, blkno);
        inode_free(fs, ino_num);
        return false;
    }

    init_dir_block(dirblk, dir->ino, fs->meta_crc);
    {
        uint32_t doff = ((kifs_shdr_t*)dirblk)->header_bytes;
        uint16_t r1 = dir_rec_len(1);
        uint16_t r2 = (uint16_t)((KIFS_BLOCK_SIZE - (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes) - doff - r1);
        r2 = (uint16_t)(r2 & ~7u);
        if (r2 < dir_rec_len(2)) {
            r2 = dir_rec_len(2);
        }
        dir_write_ent(dirblk, doff, ino_num, ".", 2, r1);
        doff += r1;
        dir_write_ent(dirblk, doff, dir->ino, "..", 2, r2);
        structured_rechecksum(dirblk, fs->meta_crc);
    }

    {
        bcache_buf_t* b = bcache_get(fs->dev, blkno);
        if (!b) {
            kfree(dirblk);
            data_block_free(fs, blkno);
            inode_free(fs, ino_num);
            return false;
        }
        memcpy(bcache_data(b), dirblk, KIFS_BLOCK_SIZE);
        bcache_mark_dirty(b);
        bcache_put(b);
    }
    kfree(dirblk);

    inode_zero(&child);
    child.type = KIFS_INO_T_DIR;
    child.mode = mode ? mode : 0755u;
    child.link_count = 2;
    child.size_bytes = 4096u;
    child.inline_extent_count = 1;
    child.inline_extents[0].file_block_start = 0;
    child.inline_extents[0].disk_block_start = blkno;
    child.inline_extents[0].block_count = 1;

    if (!inode_write(fs, ino_num, &child) ||
        !dir_add_entry(fs, dir->ino, &parent, name, ino_num, 2)) {
        data_block_free(fs, blkno);
        (void)inode_clear(fs, ino_num);
        inode_free(fs, ino_num);
        return false;
    }

    parent.link_count++;
    parent.mtime++;
    parent.ctime++;
    if (!inode_write(fs, dir->ino, &parent) || !bcache_sync_dev(fs->dev)) {
        return false;
    }

    dir->size = parent.size_bytes;
    return true;
}

static bool kifs_vnode_unlink(vnode_t* dir, const char* name) {
    kifs_fs_t* fs = NULL;
    kifs_inode_t parent;
    kifs_inode_t child;
    dir_lookup_ctx_t ctx;
    uint32_t child_ino = 0;

    if (!dir || !dir->mount || !dir->mount->fs_private || !kifs_name_valid(name)) {
        return false;
    }

    fs = (kifs_fs_t*)dir->mount->fs_private;
    if (fs->readonly || !inode_read(fs, dir->ino, &parent) || parent.type != KIFS_INO_T_DIR) {
        return false;
    }

    ctx.want = name;
    ctx.found = false;
    ctx.found_ino = 0;
    (void)dir_iterate(fs, &parent, dir_lookup_cb, &ctx);
    if (!ctx.found) {
        return false;
    }

    child_ino = ctx.found_ino;
    if (!inode_read(fs, child_ino, &child)) {
        return false;
    }

    if (child.type == KIFS_INO_T_DIR) {
        if (!dir_is_empty(fs, &child)) {
            return false;
        }
    }

    if (!dir_remove_entry(fs, &parent, name, &child_ino)) {
        return false;
    }

    if (child.type == KIFS_INO_T_DIR) {
        if (parent.link_count > 0) {
            parent.link_count--;
        }
        parent.mtime++;
        parent.ctime++;
        if (!inode_write(fs, dir->ino, &parent)) {
            return false;
        }
        child.link_count = 0;
    } else if (child.link_count > 0) {
        child.link_count--;
    }

    if (child.link_count == 0) {
        free_inode_data(fs, &child);
        if (!inode_clear(fs, child_ino)) {
            return false;
        }
        inode_free(fs, child_ino);
    } else {
        child.ctime++;
        if (!inode_write(fs, child_ino, &child)) {
            return false;
        }
    }

    return bcache_sync_dev(fs->dev);
}

static const vnode_ops_t g_kifs_vops = {
    .lookup = kifs_vnode_lookup,
    .readdir = kifs_vnode_readdir,
    .read = kifs_vnode_read,
    .write = kifs_vnode_write,
    .truncate = kifs_vnode_truncate,
    .create = kifs_vnode_create,
    .mkdir = kifs_vnode_mkdir,
    .unlink = kifs_vnode_unlink,
    .stat = kifs_vnode_stat,
};

// ---------------- public driver API ----------------

bool kifs_probe(block_device_t* dev) {
    if (!dev) return false;
    if (dev->sector_size != 512) return false;
    if (dev->total_sectors == 0) return false;

    uint32_t dev_blocks = (uint32_t)(dev->total_sectors / (BCACHE_SECTORS_PER_BLOCK));
    if (dev_blocks < 8) return false;

    return pick_superblock(dev, dev_blocks, NULL);
}

bool kifs_mount(block_device_t* dev, vfs_mount_t** out_mount) {
    kifs_superblock_t* sb = NULL;
    if (!out_mount) return false;
    *out_mount = NULL;

    if (!dev) return false;
    if (dev->sector_size != 512) return false;
    if (dev->total_sectors == 0) return false;

    uint32_t dev_blocks = (uint32_t)(dev->total_sectors / (BCACHE_SECTORS_PER_BLOCK));
    if (dev_blocks < 8) return false;

    sb = (kifs_superblock_t*)kmalloc(sizeof(*sb));
    if (!sb) return false;
    if (!pick_superblock(dev, dev_blocks, sb)) {
        kfree(sb);
        return false;
    }

    // Feature checks
    const uint32_t known_incompat = KIFS_FEAT_INCOMPAT_META_CRC | KIFS_FEAT_INCOMPAT_ORPHAN_FILE;
    if (sb->features_incompat & ~known_incompat) {
        log_errorf("kifs", "Unknown incompatible features: %x", (unsigned)(sb->features_incompat & ~known_incompat));
        kfree(sb);
        return false;
    }

    bool readonly = false;
    if (sb->features_ro_compat != 0) {
        // no known ro-compat bits yet
        readonly = true;
    }

    kifs_fs_t* fs = (kifs_fs_t*)kmalloc(sizeof(kifs_fs_t));
    if (!fs) {
        kfree(sb);
        return false;
    }
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->sb = *sb;
    fs->dev_blocks = dev_blocks;
    fs->usable_blocks = sb->usable_blocks;
    fs->data_start = sb->data_start;
    fs->data_end = sb->data_start + sb->data_blocks;
    fs->journal_start = sb->journal_start;
    fs->readonly = readonly;
    fs->meta_crc = (sb->features_incompat & KIFS_FEAT_INCOMPAT_META_CRC) != 0;

    vnode_t* root = (vnode_t*)kmalloc(sizeof(vnode_t));
    vfs_mount_t* m = (vfs_mount_t*)kmalloc(sizeof(vfs_mount_t));
    if (!root || !m) {
        if (root) kfree(root);
        if (m) kfree(m);
        kfree(fs);
        kfree(sb);
        return false;
    }

    memset(root, 0, sizeof(*root));
    memset(m, 0, sizeof(*m));

    root->mount = m;
    root->ino = sb->root_ino;
    root->ops = &g_kifs_vops;

    kifs_inode_t rino;
    if (!inode_read(fs, sb->root_ino, &rino) || rino.type != KIFS_INO_T_DIR) {
        kfree(root);
        kfree(m);
        kfree(fs);
        kfree(sb);
        return false;
    }

    root->type = VNODE_DIR;
    root->size = rino.size_bytes;

    m->fs_name = "kifs";
    m->dev = dev;
    m->root = root;
    m->readonly = readonly;
    m->fs_private = fs;

    // Do not write dirty superblocks just to mount. Removable-media mount
    // support must not depend on write traffic before USB writes are proven.

    kfree(sb);
    *out_mount = m;
    return true;
}

// ---------------- mkfs (simple) ----------------

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

bool kifs_debug_get_superblock(const vfs_mount_t* m, void* out_superblock_4k) {
    if (!m || !out_superblock_4k || !m->fs_private) return false;
    const kifs_fs_t* fs = (const kifs_fs_t*)m->fs_private;
    memcpy(out_superblock_4k, &fs->sb, sizeof(kifs_superblock_t));
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

bool kifs_mkfs_ex(block_device_t* dev, uint32_t inode_count, bool create_kiwios_root) {
    static const char* base_dir_names[] = { "bin", "dev", "mnt", "home", "tmp" };
    const uint32_t base_dir_count = create_kiwios_root ? 5u : 0u;
    const uint32_t data_blocks_needed = 1u + base_dir_count;

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
        if (data_start + data_blocks_needed <= usable_blocks) {
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
            enum {
                KIFS_MKFS_ROOT_INO = 1,
                KIFS_MKFS_ORPHAN_INO = 2,
                KIFS_MKFS_BIN_INO = 3,
                KIFS_MKFS_DEV_INO = 4,
                KIFS_MKFS_MNT_INO = 5,
                KIFS_MKFS_HOME_INO = 6,
                KIFS_MKFS_TMP_INO = 7,
            };
            uint32_t root_dir_blk = data_start;
            uint32_t dir_blks[5] = {0};
            uint8_t* data_mem = NULL;
            kifs_superblock_t* sb_tmp = NULL;
            uint32_t data_mem_bytes = data_blocks_needed * KIFS_BLOCK_SIZE;

            for (uint32_t i = 0; i < base_dir_count; i++) {
                dir_blks[i] = data_start + 1u + i;
            }

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
            for (uint32_t i = 0; i < base_dir_count; i++) bitmap_set_in_blocks(bb_mem, bb_blocks, dir_blks[i]);

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
            }
            bitmap_set_in_blocks(ib_mem, ib_blocks, 0); // reserved
            bitmap_set_in_blocks(ib_mem, ib_blocks, KIFS_MKFS_ROOT_INO);
            bitmap_set_in_blocks(ib_mem, ib_blocks, KIFS_MKFS_ORPHAN_INO);
            for (uint32_t i = 0; i < base_dir_count; i++) {
                bitmap_set_in_blocks(ib_mem, ib_blocks, KIFS_MKFS_BIN_INO + i);
            }

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
            root.link_count = 2 + base_dir_count;
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

            kifs_inode_t dirs[5];
            for (uint32_t i = 0; i < base_dir_count; i++) {
                inode_zero(&dirs[i]);
                dirs[i].type = KIFS_INO_T_DIR;
                dirs[i].mode = 0755;
                dirs[i].link_count = 2;
                dirs[i].size_bytes = 4096;
                dirs[i].inline_extent_count = 1;
                dirs[i].inline_extents[0].file_block_start = 0;
                dirs[i].inline_extents[0].disk_block_start = dir_blks[i];
                dirs[i].inline_extents[0].block_count = 1;
            }

            // Write inodes into table memory
            memcpy(it_mem + (KIFS_MKFS_ROOT_INO * 256u), &root, sizeof(root));
            memcpy(it_mem + (KIFS_MKFS_ORPHAN_INO * 256u), &orphan, sizeof(orphan));
            for (uint32_t i = 0; i < base_dir_count; i++) {
                memcpy(it_mem + ((KIFS_MKFS_BIN_INO + i) * 256u), &dirs[i], sizeof(dirs[i]));
            }

            data_mem = (uint8_t*)kmalloc(data_mem_bytes);
            if (!data_mem) {
                kfree(bb_mem);
                kfree(ib_mem);
                kfree(it_mem);
                return false;
            }
            memset(data_mem, 0, data_mem_bytes);

            // --- root directory block ---
            {
                uint8_t* dirblk = data_mem;
                uint32_t doff = 0;
                uint32_t dir_end = 0;

                init_dir_block(dirblk, 1, true);
                doff = (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes;
                dir_end = (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes +
                          (uint32_t)((kifs_shdr_t*)dirblk)->payload_bytes;

                dir_write_ent(dirblk, doff, KIFS_MKFS_ROOT_INO, ".", 2, dir_rec_len(1));
                doff += dir_rec_len(1);
                {
                    uint16_t min_rec = dir_rec_len(2);
                    uint16_t rec = min_rec;
                    if (base_dir_count == 0u && dir_end > doff) {
                        rec = (uint16_t)((dir_end - doff) & ~7u);
                        if (rec < min_rec) {
                            rec = min_rec;
                        }
                    }
                    dir_write_ent(dirblk, doff, KIFS_MKFS_ROOT_INO, "..", 2, rec);
                    doff += rec;
                }

                for (uint32_t i = 0; i < base_dir_count; i++) {
                    uint16_t min_rec = dir_rec_len((uint8_t)strlen(base_dir_names[i]));
                    uint16_t rec = min_rec;
                    if ((i + 1u) == base_dir_count && dir_end > doff) {
                        rec = (uint16_t)((dir_end - doff) & ~7u);
                        if (rec < min_rec) {
                            rec = min_rec;
                        }
                    }
                    dir_write_ent(dirblk, doff, KIFS_MKFS_BIN_INO + i, base_dir_names[i], 2, rec);
                    doff += rec;
                }

                structured_rechecksum(dirblk, true);
            }

            // --- base directory blocks ---
            for (uint32_t i = 0; i < base_dir_count; i++) {
                uint8_t* dirblk = data_mem + ((i + 1u) * 4096u);
                uint32_t doff = 0;
                uint16_t r1 = 0;
                uint16_t r2 = 0;

                init_dir_block(dirblk, KIFS_MKFS_ROOT_INO, true);
                doff = (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes;

                r1 = dir_rec_len(1);
                dir_write_ent(dirblk, doff, KIFS_MKFS_BIN_INO + i, ".", 2, r1);
                doff += r1;

                r2 = (uint16_t)((4096u - (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes) - doff);
                r2 = (uint16_t)(r2 & ~7u);
                if (r2 < dir_rec_len(2)) r2 = dir_rec_len(2);
                dir_write_ent(dirblk, doff, KIFS_MKFS_ROOT_INO, "..", 2, r2);

                structured_rechecksum(dirblk, true);
            }

            // --- superblock blocks ---
            sb_tmp = (kifs_superblock_t*)kmalloc(sizeof(*sb_tmp));
            if (!sb_tmp) {
                kfree(bb_mem);
                kfree(ib_mem);
                kfree(it_mem);
                kfree(data_mem);
                return false;
            }

            // Write everything
            *sb_tmp = sb;
            sb_tmp->sb_blockno = 0;
            sb_tmp->h.checksum = 0;
            sb_tmp->h.checksum = crc32_ieee(sb_tmp, KIFS_BLOCK_SIZE);
            write_block_raw(dev, 0, (const uint8_t*)sb_tmp);

            *sb_tmp = sb;
            sb_tmp->sb_blockno = 1;
            sb_tmp->h.checksum = 0;
            sb_tmp->h.checksum = crc32_ieee(sb_tmp, KIFS_BLOCK_SIZE);
            write_block_raw(dev, 1, (const uint8_t*)sb_tmp);

            for (uint32_t i = 0; i < bb_blocks; i++) {
                write_block_raw(dev, sb.block_bitmap_start + i, bb_mem + i * 4096u);
            }
            for (uint32_t i = 0; i < ib_blocks; i++) {
                write_block_raw(dev, sb.inode_bitmap_start + i, ib_mem + i * 4096u);
            }
            for (uint32_t i = 0; i < it_blocks; i++) {
                write_block_raw(dev, sb.inode_table_start + i, it_mem + i * 4096u);
            }

            write_block_raw(dev, root_dir_blk, data_mem);
            for (uint32_t i = 0; i < base_dir_count; i++) {
                write_block_raw(dev, dir_blks[i], data_mem + ((i + 1u) * 4096u));
            }

            bcache_sync_dev(dev);

            kfree(bb_mem);
            kfree(ib_mem);
            kfree(it_mem);
            kfree(data_mem);
            kfree(sb_tmp);

            log_okf("kifs", "mkfs complete: blocks=%u usable=%u inodes=%u data_start=%u layout=%s",
                    N,
                    usable_blocks,
                    inode_count,
                    data_start,
                    create_kiwios_root ? "kiwios-root" : "minimal");
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

bool kifs_mkfs(block_device_t* dev, uint32_t inode_count) {
    return kifs_mkfs_ex(dev, inode_count, false);
}
