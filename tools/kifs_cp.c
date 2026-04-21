#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define SECTOR_SIZE 512u
#define KIFS_BLOCK_SIZE 4096u
#define KIFS_BITMAP_PAYLOAD_BYTES (KIFS_BLOCK_SIZE - (uint32_t)sizeof(kifs_shdr_t))
#define KIFS_BITMAP_BITS_PER_BLOCK (KIFS_BITMAP_PAYLOAD_BYTES * 8u)

#define KIFS_MAGIC_SUPER "KIFS"
#define KIFS_MAGIC_INODE "KINO"
#define KIFS_MAGIC_DIR   "KDIR"
#define KIFS_MAGIC_BMB   "KBMB"
#define KIFS_MAGIC_IMB   "KIMB"

#define KIFS_STRUCT_VERSION 1u
#define KIFS_FEAT_INCOMPAT_META_CRC    (1u << 0)
#define KIFS_FEAT_INCOMPAT_ORPHAN_FILE (1u << 1)

enum {
    KIFS_BTYPE_SUPER = 1,
    KIFS_BTYPE_BMB   = 2,
    KIFS_BTYPE_IMB   = 3,
    KIFS_BTYPE_DIR   = 4,
};

enum {
    KIFS_INO_T_INVALID = 0,
    KIFS_INO_T_FILE    = 1,
    KIFS_INO_T_DIR     = 2,
};

typedef struct __attribute__((packed)) {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} gpt_entry_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t block_type;
    uint16_t header_bytes;
    uint16_t payload_bytes;
    uint32_t flags;
    uint32_t checksum;
} kifs_shdr_t;

typedef struct __attribute__((packed)) {
    kifs_shdr_t h;
    uint16_t fs_major;
    uint16_t fs_minor;
    uint32_t block_size;
    uint32_t sb_blockno;
    uint32_t dirty;
    uint32_t features_compat;
    uint32_t features_ro_compat;
    uint32_t features_incompat;
    uint32_t total_blocks;
    uint32_t usable_blocks;
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;
    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t inode_count;
    uint32_t root_ino;
    uint32_t orphan_ino;
    uint64_t sb_seq;
    uint64_t mount_count;
    uint64_t last_mount_time;
    uint8_t pad[3964];
} kifs_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t file_block_start;
    uint32_t disk_block_start;
    uint32_t block_count;
} kifs_extent_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t link_count;
    uint64_t size_bytes;
    uint64_t mtime;
    uint64_t ctime;
    uint16_t inline_extent_count;
    uint16_t extent_tree_height;
    uint32_t extent_tree_root;
    kifs_extent_t inline_extents[8];
    uint32_t inode_checksum;
    uint8_t reserved[100];
} kifs_inode_t;

typedef struct {
    FILE* fp;
    uint64_t part_offset;
    uint64_t part_blocks;
    kifs_superblock_t sb;
    bool meta_crc;
    uint8_t* block_bitmap;
    uint8_t* inode_bitmap;
    uint8_t* inode_table;
} kifs_image_t;

typedef struct {
    bool found;
    uint32_t ino;
    uint32_t entry_off;
    uint16_t entry_reclen;
    uint32_t last_off;
    uint16_t last_reclen;
    uint16_t last_min_reclen;
} dir_scan_t;

static uint32_t crc32_ieee(const void* data, size_t len) {
    static uint32_t table[256];
    static int table_init = 0;
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (!table_init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1u) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            table[i] = c;
        }
        table_init = 1;
    }

    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFu;
}

static int read_at(FILE* fp, uint64_t off, void* buf, size_t len) {
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0) {
        return -1;
    }
    return fread(buf, 1, len, fp) == len ? 0 : -1;
}

static int write_at(FILE* fp, uint64_t off, const void* buf, size_t len) {
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0) {
        return -1;
    }
    return fwrite(buf, 1, len, fp) == len ? 0 : -1;
}

static bool read_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    FILE* fp = NULL;
    long size = 0;
    uint8_t* buf = NULL;

    *out_buf = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "kifs_cp: failed to open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    buf = (uint8_t*)malloc((size_t)size);
    if (!buf) {
        fclose(fp);
        return false;
    }

    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return false;
    }

    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)size;
    return true;
}

static bool gpt_entry_is_empty(const gpt_entry_t* ent) {
    for (size_t i = 0; i < sizeof(ent->type_guid); i++) {
        if (ent->type_guid[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool gpt_get_partition(FILE* fp, uint32_t part_number, uint64_t* out_first_lba, uint64_t* out_last_lba) {
    gpt_header_t hdr;
    gpt_entry_t ent;
    uint64_t ent_off = 0;

    if (part_number == 0) {
        fprintf(stderr, "kifs_cp: partition numbers are 1-based\n");
        return false;
    }

    if (read_at(fp, SECTOR_SIZE, &hdr, sizeof(hdr)) != 0) {
        fprintf(stderr, "kifs_cp: failed to read GPT header\n");
        return false;
    }

    if (memcmp(hdr.signature, "EFI PART", 8) != 0) {
        fprintf(stderr, "kifs_cp: disk image does not contain a GPT header\n");
        return false;
    }

    if (part_number > hdr.num_partition_entries) {
        fprintf(stderr, "kifs_cp: partition %" PRIu32 " is out of range\n", part_number);
        return false;
    }

    if (hdr.sizeof_partition_entry < sizeof(gpt_entry_t)) {
        fprintf(stderr, "kifs_cp: unsupported GPT entry size\n");
        return false;
    }

    ent_off = (hdr.partition_entry_lba * SECTOR_SIZE) + ((uint64_t)(part_number - 1u) * hdr.sizeof_partition_entry);
    if (read_at(fp, ent_off, &ent, sizeof(ent)) != 0) {
        fprintf(stderr, "kifs_cp: failed to read GPT entry %" PRIu32 "\n", part_number);
        return false;
    }

    if (gpt_entry_is_empty(&ent) || ent.first_lba == 0 || ent.last_lba < ent.first_lba) {
        fprintf(stderr, "kifs_cp: GPT entry %" PRIu32 " is empty\n", part_number);
        return false;
    }

    *out_first_lba = ent.first_lba;
    *out_last_lba = ent.last_lba;
    return true;
}

static bool magic_eq(const char got[4], const char want[4]) {
    return got[0] == want[0] &&
           got[1] == want[1] &&
           got[2] == want[2] &&
           got[3] == want[3];
}

static bool validate_shdr(const uint8_t* block, const char magic[4], uint16_t block_type, bool require_crc) {
    const kifs_shdr_t* h = (const kifs_shdr_t*)block;
    if (!magic_eq(h->magic, magic)) {
        return false;
    }
    if (h->version != KIFS_STRUCT_VERSION || h->block_type != block_type) {
        return false;
    }
    if (h->header_bytes < sizeof(kifs_shdr_t) || h->header_bytes > KIFS_BLOCK_SIZE) {
        return false;
    }
    if ((uint32_t)h->header_bytes + (uint32_t)h->payload_bytes > KIFS_BLOCK_SIZE) {
        return false;
    }
    if (require_crc) {
        uint8_t tmp[KIFS_BLOCK_SIZE];
        uint32_t saved_crc = h->checksum;
        memcpy(tmp, block, sizeof(tmp));
        ((kifs_shdr_t*)tmp)->checksum = 0;
        if (crc32_ieee(tmp, sizeof(tmp)) != saved_crc) {
            return false;
        }
    }
    return true;
}

static bool sb_layout_sane(const kifs_superblock_t* sb, uint32_t dev_blocks) {
    uint32_t bb_end = 0;
    uint32_t ib_end = 0;
    uint32_t it_end = 0;
    uint32_t d_end = 0;
    uint32_t expected_it_blocks = 0;

    if (!sb || sb->block_size != KIFS_BLOCK_SIZE) {
        return false;
    }
    if (sb->total_blocks == 0 || sb->total_blocks > dev_blocks) {
        return false;
    }
    if (sb->usable_blocks > sb->total_blocks) {
        return false;
    }
    if (sb->journal_start != sb->usable_blocks) {
        return false;
    }
    if (sb->journal_start + sb->journal_blocks != sb->total_blocks) {
        return false;
    }

    bb_end = sb->block_bitmap_start + sb->block_bitmap_blocks;
    ib_end = sb->inode_bitmap_start + sb->inode_bitmap_blocks;
    it_end = sb->inode_table_start + sb->inode_table_blocks;
    d_end = sb->data_start + sb->data_blocks;

    if (sb->block_bitmap_start < 2 || bb_end > sb->usable_blocks) {
        return false;
    }
    if (sb->inode_bitmap_start < bb_end || ib_end > sb->usable_blocks) {
        return false;
    }
    if (sb->inode_table_start < ib_end || it_end > sb->usable_blocks) {
        return false;
    }
    if (sb->data_start < it_end || d_end != sb->usable_blocks) {
        return false;
    }

    expected_it_blocks = (sb->inode_count + 15u) / 16u;
    if (expected_it_blocks != sb->inode_table_blocks) {
        return false;
    }

    return sb->root_ino == 1 && sb->inode_count > 1;
}

static bool read_block(const kifs_image_t* fs, uint32_t blkno, uint8_t out[KIFS_BLOCK_SIZE]) {
    return read_at(fs->fp, fs->part_offset + ((uint64_t)blkno * KIFS_BLOCK_SIZE), out, KIFS_BLOCK_SIZE) == 0;
}

static bool write_block(const kifs_image_t* fs, uint32_t blkno, const uint8_t in[KIFS_BLOCK_SIZE]) {
    return write_at(fs->fp, fs->part_offset + ((uint64_t)blkno * KIFS_BLOCK_SIZE), in, KIFS_BLOCK_SIZE) == 0;
}

static bool validate_superblock_at(const kifs_image_t* fs, uint32_t blkno, kifs_superblock_t* out) {
    uint8_t buf[KIFS_BLOCK_SIZE];
    if (!read_block(fs, blkno, buf)) {
        return false;
    }
    if (!validate_shdr(buf, KIFS_MAGIC_SUPER, KIFS_BTYPE_SUPER, true)) {
        return false;
    }
    if (((const kifs_superblock_t*)buf)->sb_blockno != blkno) {
        return false;
    }
    if (!sb_layout_sane((const kifs_superblock_t*)buf, (uint32_t)fs->part_blocks)) {
        return false;
    }
    memcpy(out, buf, sizeof(*out));
    return true;
}

static bool kifs_open_image(kifs_image_t* fs, const char* disk_path, uint32_t part_number) {
    uint64_t first_lba = 0;
    uint64_t last_lba = 0;
    kifs_superblock_t a;
    kifs_superblock_t b;
    bool va = false;
    bool vb = false;
    uint32_t bb_bytes = 0;
    uint32_t ib_bytes = 0;
    uint32_t it_bytes = 0;
    uint32_t known_incompat = KIFS_FEAT_INCOMPAT_META_CRC | KIFS_FEAT_INCOMPAT_ORPHAN_FILE;

    memset(fs, 0, sizeof(*fs));
    fs->fp = fopen(disk_path, "rb+");
    if (!fs->fp) {
        fprintf(stderr, "kifs_cp: failed to open %s: %s\n", disk_path, strerror(errno));
        return false;
    }

    if (!gpt_get_partition(fs->fp, part_number, &first_lba, &last_lba)) {
        fclose(fs->fp);
        fs->fp = NULL;
        return false;
    }

    fs->part_offset = first_lba * SECTOR_SIZE;
    fs->part_blocks = ((last_lba - first_lba) + 1u) * SECTOR_SIZE / KIFS_BLOCK_SIZE;

    va = validate_superblock_at(fs, 0, &a);
    vb = validate_superblock_at(fs, 1, &b);
    if (!va && !vb) {
        fprintf(stderr, "kifs_cp: no valid KiFS superblock found in partition %" PRIu32 "\n", part_number);
        fclose(fs->fp);
        fs->fp = NULL;
        return false;
    }

    if (va && (!vb || a.sb_seq >= b.sb_seq)) {
        fs->sb = a;
    } else {
        fs->sb = b;
    }

    if ((fs->sb.features_incompat & ~known_incompat) != 0) {
        fprintf(stderr, "kifs_cp: unsupported KiFS incompatible feature bits: 0x%x\n",
                fs->sb.features_incompat & ~known_incompat);
        fclose(fs->fp);
        fs->fp = NULL;
        return false;
    }

    fs->meta_crc = (fs->sb.features_incompat & KIFS_FEAT_INCOMPAT_META_CRC) != 0;

    bb_bytes = fs->sb.block_bitmap_blocks * KIFS_BLOCK_SIZE;
    ib_bytes = fs->sb.inode_bitmap_blocks * KIFS_BLOCK_SIZE;
    it_bytes = fs->sb.inode_table_blocks * KIFS_BLOCK_SIZE;

    fs->block_bitmap = (uint8_t*)malloc(bb_bytes);
    fs->inode_bitmap = (uint8_t*)malloc(ib_bytes);
    fs->inode_table = (uint8_t*)malloc(it_bytes);
    if (!fs->block_bitmap || !fs->inode_bitmap || !fs->inode_table) {
        fprintf(stderr, "kifs_cp: out of memory loading KiFS metadata\n");
        fclose(fs->fp);
        free(fs->block_bitmap);
        free(fs->inode_bitmap);
        free(fs->inode_table);
        memset(fs, 0, sizeof(*fs));
        return false;
    }

    for (uint32_t i = 0; i < fs->sb.block_bitmap_blocks; i++) {
        if (!read_block(fs, fs->sb.block_bitmap_start + i, fs->block_bitmap + (i * KIFS_BLOCK_SIZE)) ||
            !validate_shdr(fs->block_bitmap + (i * KIFS_BLOCK_SIZE), KIFS_MAGIC_BMB, KIFS_BTYPE_BMB, true)) {
            fprintf(stderr, "kifs_cp: failed to load block bitmap block %" PRIu32 "\n", i);
            fclose(fs->fp);
            free(fs->block_bitmap);
            free(fs->inode_bitmap);
            free(fs->inode_table);
            memset(fs, 0, sizeof(*fs));
            return false;
        }
    }

    for (uint32_t i = 0; i < fs->sb.inode_bitmap_blocks; i++) {
        if (!read_block(fs, fs->sb.inode_bitmap_start + i, fs->inode_bitmap + (i * KIFS_BLOCK_SIZE)) ||
            !validate_shdr(fs->inode_bitmap + (i * KIFS_BLOCK_SIZE), KIFS_MAGIC_IMB, KIFS_BTYPE_IMB, true)) {
            fprintf(stderr, "kifs_cp: failed to load inode bitmap block %" PRIu32 "\n", i);
            fclose(fs->fp);
            free(fs->block_bitmap);
            free(fs->inode_bitmap);
            free(fs->inode_table);
            memset(fs, 0, sizeof(*fs));
            return false;
        }
    }

    for (uint32_t i = 0; i < fs->sb.inode_table_blocks; i++) {
        if (!read_block(fs, fs->sb.inode_table_start + i, fs->inode_table + (i * KIFS_BLOCK_SIZE))) {
            fprintf(stderr, "kifs_cp: failed to load inode table block %" PRIu32 "\n", i);
            fclose(fs->fp);
            free(fs->block_bitmap);
            free(fs->inode_bitmap);
            free(fs->inode_table);
            memset(fs, 0, sizeof(*fs));
            return false;
        }
    }

    return true;
}

static void kifs_close_image(kifs_image_t* fs) {
    if (!fs) {
        return;
    }
    if (fs->fp) {
        fflush(fs->fp);
        fclose(fs->fp);
    }
    free(fs->block_bitmap);
    free(fs->inode_bitmap);
    free(fs->inode_table);
    memset(fs, 0, sizeof(*fs));
}

static uint8_t* bitmap_payload(uint8_t* bitmap_mem, uint32_t block_index) {
    return bitmap_mem + ((uint64_t)block_index * KIFS_BLOCK_SIZE) + sizeof(kifs_shdr_t);
}

static bool bitmap_get_mem(const uint8_t* bitmap_mem, uint32_t bitmap_blocks, uint32_t bit_index) {
    uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
    const uint8_t* payload = NULL;
    if (bi >= bitmap_blocks) {
        return true;
    }
    payload = bitmap_mem + ((uint64_t)bi * KIFS_BLOCK_SIZE) + sizeof(kifs_shdr_t);
    return (payload[bit / 8u] & (uint8_t)(1u << (bit % 8u))) != 0;
}

static void bitmap_set_mem(uint8_t* bitmap_mem, uint32_t bitmap_blocks, uint32_t bit_index) {
    uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
    uint8_t* payload = NULL;
    if (bi >= bitmap_blocks) {
        return;
    }
    payload = bitmap_payload(bitmap_mem, bi);
    payload[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static void bitmap_clear_mem(uint8_t* bitmap_mem, uint32_t bitmap_blocks, uint32_t bit_index) {
    uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
    uint8_t* payload = NULL;
    if (bi >= bitmap_blocks) {
        return;
    }
    payload = bitmap_payload(bitmap_mem, bi);
    payload[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
}

static kifs_inode_t* inode_ptr(kifs_image_t* fs, uint32_t ino) {
    if (!fs || ino >= fs->sb.inode_count) {
        return NULL;
    }
    return (kifs_inode_t*)(fs->inode_table + ((uint64_t)ino * sizeof(kifs_inode_t)));
}

static void inode_zero(kifs_inode_t* ino) {
    memset(ino, 0, sizeof(*ino));
    memcpy(ino->magic, KIFS_MAGIC_INODE, 4);
    ino->version = 1;
}

static uint16_t dir_rec_len(uint8_t name_len) {
    uint32_t n = 8u + (uint32_t)name_len;
    return (uint16_t)((n + 7u) & ~7u);
}

static void dir_write_entry(uint8_t* blk, uint32_t off, uint32_t ino, const char* name, uint8_t ftype, uint16_t rec_len) {
    uint8_t name_len = (uint8_t)strlen(name);
    uint8_t* p = blk + off;
    memset(p, 0, rec_len);
    *(uint32_t*)(p + 0) = ino;
    *(uint16_t*)(p + 4) = rec_len;
    *(uint8_t*)(p + 6) = name_len;
    *(uint8_t*)(p + 7) = ftype;
    memcpy(p + 8, name, name_len);
}

static bool find_free_inode(kifs_image_t* fs, uint32_t* out_ino) {
    for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
        if (!bitmap_get_mem(fs->inode_bitmap, fs->sb.inode_bitmap_blocks, ino)) {
            *out_ino = ino;
            return true;
        }
    }
    return false;
}

static void free_inode_blocks(kifs_image_t* fs, const kifs_inode_t* ino) {
    if (!fs || !ino) {
        return;
    }
    if (ino->extent_tree_height != 0) {
        return;
    }
    for (uint16_t i = 0; i < ino->inline_extent_count && i < 8; i++) {
        const kifs_extent_t* ext = &ino->inline_extents[i];
        for (uint32_t j = 0; j < ext->block_count; j++) {
            bitmap_clear_mem(fs->block_bitmap, fs->sb.block_bitmap_blocks, ext->disk_block_start + j);
        }
    }
}

static bool allocate_extents(kifs_image_t* fs, uint32_t need_blocks, kifs_extent_t out_extents[8], uint16_t* out_count) {
    uint32_t remaining = need_blocks;
    uint32_t current_start = 0;
    uint32_t current_len = 0;
    uint16_t ext_count = 0;

    memset(out_extents, 0, sizeof(kifs_extent_t) * 8u);

    if (need_blocks == 0) {
        *out_count = 0;
        return true;
    }

    for (uint32_t blk = fs->sb.data_start; blk < fs->sb.data_start + fs->sb.data_blocks && remaining > 0; blk++) {
        if (bitmap_get_mem(fs->block_bitmap, fs->sb.block_bitmap_blocks, blk)) {
            if (current_len != 0) {
                if (ext_count >= 8) {
                    return false;
                }
                out_extents[ext_count].file_block_start = need_blocks - remaining - current_len;
                out_extents[ext_count].disk_block_start = current_start;
                out_extents[ext_count].block_count = current_len;
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
            if (ext_count >= 8) {
                return false;
            }
            out_extents[ext_count].file_block_start = need_blocks - remaining - current_len;
            out_extents[ext_count].disk_block_start = current_start;
            out_extents[ext_count].block_count = current_len;
            ext_count++;
            current_start = blk;
            current_len = 1;
        }

        remaining--;
    }

    if (current_len != 0) {
        if (ext_count >= 8) {
            return false;
        }
        out_extents[ext_count].file_block_start = need_blocks - remaining - current_len;
        out_extents[ext_count].disk_block_start = current_start;
        out_extents[ext_count].block_count = current_len;
        ext_count++;
    }

    if (remaining != 0) {
        return false;
    }

    for (uint16_t i = 0; i < ext_count; i++) {
        for (uint32_t j = 0; j < out_extents[i].block_count; j++) {
            bitmap_set_mem(fs->block_bitmap, fs->sb.block_bitmap_blocks, out_extents[i].disk_block_start + j);
        }
    }

    *out_count = ext_count;
    return true;
}

static bool find_root_dir_block(const kifs_image_t* fs, uint32_t* out_block) {
    kifs_inode_t root = {0};
    if (!fs || !out_block) {
        return false;
    }

    memcpy(&root, inode_ptr((kifs_image_t*)fs, fs->sb.root_ino), sizeof(root));
    if (!magic_eq(root.magic, KIFS_MAGIC_INODE) || root.type != KIFS_INO_T_DIR || root.inline_extent_count == 0) {
        return false;
    }

    if (root.inline_extents[0].file_block_start != 0 || root.inline_extents[0].block_count == 0) {
        return false;
    }

    *out_block = root.inline_extents[0].disk_block_start;
    return true;
}

static bool scan_dir_block(const uint8_t* blk, const char* name, dir_scan_t* out) {
    const kifs_shdr_t* h = (const kifs_shdr_t*)blk;
    uint32_t off = h->header_bytes;
    uint32_t end = h->header_bytes + h->payload_bytes;

    memset(out, 0, sizeof(*out));

    while (off + 8u <= end) {
        const uint8_t* p = blk + off;
        uint32_t ino = *(const uint32_t*)(p + 0);
        uint16_t reclen = *(const uint16_t*)(p + 4);
        uint8_t namelen = *(const uint8_t*)(p + 6);
        uint16_t min_rec = dir_rec_len(namelen);

        if (reclen < min_rec || (reclen & 7u) != 0 || off + reclen > end) {
            break;
        }

        if (ino != 0) {
            char cur_name[256];
            memcpy(cur_name, p + 8, namelen);
            cur_name[namelen] = '\0';

            out->last_off = off;
            out->last_reclen = reclen;
            out->last_min_reclen = min_rec;

            if (strcmp(cur_name, name) == 0) {
                out->found = true;
                out->ino = ino;
                out->entry_off = off;
                out->entry_reclen = reclen;
            }
        }

        off += reclen;
    }

    return true;
}

static bool update_dir_checksum(kifs_image_t* fs, uint8_t dirblk[KIFS_BLOCK_SIZE]) {
    kifs_shdr_t* h = (kifs_shdr_t*)dirblk;
    h->checksum = 0;
    if (fs->meta_crc) {
        h->checksum = crc32_ieee(dirblk, KIFS_BLOCK_SIZE);
    }
    return true;
}

static bool ensure_root_entry(kifs_image_t* fs, const char* name, uint32_t ino, bool overwrite_existing) {
    uint8_t dirblk[KIFS_BLOCK_SIZE];
    dir_scan_t scan;
    uint32_t dir_block = 0;
    uint16_t new_reclen = 0;

    if (!find_root_dir_block(fs, &dir_block)) {
        fprintf(stderr, "kifs_cp: unsupported root directory layout\n");
        return false;
    }

    if (!read_block(fs, dir_block, dirblk) || !validate_shdr(dirblk, KIFS_MAGIC_DIR, KIFS_BTYPE_DIR, fs->meta_crc)) {
        fprintf(stderr, "kifs_cp: failed to read root directory block\n");
        return false;
    }

    scan_dir_block(dirblk, name, &scan);
    if (scan.found) {
        if (!overwrite_existing || scan.ino != ino) {
            return true;
        }
    } else {
        new_reclen = dir_rec_len((uint8_t)strlen(name));
        if (scan.last_reclen < scan.last_min_reclen || (uint16_t)(scan.last_reclen - scan.last_min_reclen) < new_reclen) {
            fprintf(stderr, "kifs_cp: root directory block is full\n");
            return false;
        }

        *(uint16_t*)(dirblk + scan.last_off + 4u) = scan.last_min_reclen;
        dir_write_entry(dirblk,
                        scan.last_off + scan.last_min_reclen,
                        ino,
                        name,
                        1,
                        (uint16_t)(scan.last_reclen - scan.last_min_reclen));
    }

    update_dir_checksum(fs, dirblk);
    if (!write_block(fs, dir_block, dirblk)) {
        fprintf(stderr, "kifs_cp: failed to write root directory block\n");
        return false;
    }

    return true;
}

static bool write_file_data(kifs_image_t* fs, const uint8_t* data, size_t len, const kifs_extent_t* extents, uint16_t extent_count) {
    size_t written = 0;
    for (uint16_t i = 0; i < extent_count; i++) {
        for (uint32_t j = 0; j < extents[i].block_count; j++) {
            uint8_t blk[KIFS_BLOCK_SIZE];
            size_t chunk = len - written;
            if (chunk > KIFS_BLOCK_SIZE) {
                chunk = KIFS_BLOCK_SIZE;
            }
            memset(blk, 0, sizeof(blk));
            if (chunk > 0) {
                memcpy(blk, data + written, chunk);
                written += chunk;
            }
            if (!write_block(fs, extents[i].disk_block_start + j, blk)) {
                return false;
            }
        }
    }
    return true;
}

static bool flush_metadata(kifs_image_t* fs) {
    for (uint32_t i = 0; i < fs->sb.block_bitmap_blocks; i++) {
        kifs_shdr_t* h = (kifs_shdr_t*)(fs->block_bitmap + ((uint64_t)i * KIFS_BLOCK_SIZE));
        h->checksum = 0;
        h->checksum = crc32_ieee(h, KIFS_BLOCK_SIZE);
        if (!write_block(fs, fs->sb.block_bitmap_start + i, (const uint8_t*)h)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < fs->sb.inode_bitmap_blocks; i++) {
        kifs_shdr_t* h = (kifs_shdr_t*)(fs->inode_bitmap + ((uint64_t)i * KIFS_BLOCK_SIZE));
        h->checksum = 0;
        h->checksum = crc32_ieee(h, KIFS_BLOCK_SIZE);
        if (!write_block(fs, fs->sb.inode_bitmap_start + i, (const uint8_t*)h)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < fs->sb.inode_table_blocks; i++) {
        if (!write_block(fs, fs->sb.inode_table_start + i, fs->inode_table + ((uint64_t)i * KIFS_BLOCK_SIZE))) {
            return false;
        }
    }

    fs->sb.sb_seq++;
    fs->sb.dirty = 0;

    for (uint32_t sbno = 0; sbno <= 1; sbno++) {
        kifs_superblock_t sb = fs->sb;
        sb.sb_blockno = sbno;
        sb.h.checksum = 0;
        sb.h.checksum = crc32_ieee(&sb, sizeof(sb));
        if (!write_block(fs, sbno, (const uint8_t*)&sb)) {
            return false;
        }
    }

    return fflush(fs->fp) == 0;
}

static bool extract_root_name(const char* dst_path, char name_out[256]) {
    size_t len = 0;
    if (!dst_path || dst_path[0] != '/') {
        return false;
    }
    if (dst_path[1] == '\0') {
        return false;
    }

    dst_path++;
    len = strlen(dst_path);
    if (len == 0 || len > 255 || strchr(dst_path, '/') != NULL) {
        return false;
    }

    memcpy(name_out, dst_path, len + 1u);
    return true;
}

int main(int argc, char** argv) {
    kifs_image_t fs;
    uint8_t* src = NULL;
    size_t src_len = 0;
    char root_name[256];
    uint32_t ino = 0;
    bool new_inode = false;
    kifs_inode_t old_inode;
    kifs_extent_t extents[8];
    uint16_t extent_count = 0;
    kifs_inode_t* ino_slot = NULL;
    uint32_t need_blocks = 0;

    if (argc != 5) {
        fprintf(stderr, "Usage: %s <disk.img> <partition_number> <src_file> </root_name>\n", argv[0]);
        fprintf(stderr, "Example: %s disk_gpt.img 1 userspace/bin/hello /hello\n", argv[0]);
        return 1;
    }

    if (!extract_root_name(argv[4], root_name)) {
        fprintf(stderr, "kifs_cp: destination must be a root-level absolute path like /hello\n");
        return 1;
    }

    if (!read_file(argv[3], &src, &src_len)) {
        fprintf(stderr, "kifs_cp: failed to read source file %s\n", argv[3]);
        return 1;
    }

    if (!kifs_open_image(&fs, argv[1], (uint32_t)strtoul(argv[2], NULL, 10))) {
        free(src);
        return 1;
    }

    {
        uint8_t dirblk[KIFS_BLOCK_SIZE];
        dir_scan_t scan;
        uint32_t dir_block = 0;

        if (!find_root_dir_block(&fs, &dir_block) ||
            !read_block(&fs, dir_block, dirblk) ||
            !validate_shdr(dirblk, KIFS_MAGIC_DIR, KIFS_BTYPE_DIR, fs.meta_crc)) {
            fprintf(stderr, "kifs_cp: failed to inspect root directory\n");
            kifs_close_image(&fs);
            free(src);
            return 1;
        }

        scan_dir_block(dirblk, root_name, &scan);
        if (scan.found) {
            ino = scan.ino;
        } else {
            if (!find_free_inode(&fs, &ino)) {
                fprintf(stderr, "kifs_cp: no free inode available\n");
                kifs_close_image(&fs);
                free(src);
                return 1;
            }
            new_inode = true;
        }
    }

    ino_slot = inode_ptr(&fs, ino);
    if (!ino_slot) {
        fprintf(stderr, "kifs_cp: invalid inode slot\n");
        kifs_close_image(&fs);
        free(src);
        return 1;
    }

    memset(&old_inode, 0, sizeof(old_inode));
    memcpy(&old_inode, ino_slot, sizeof(old_inode));
    if (!new_inode) {
        if (!magic_eq(old_inode.magic, KIFS_MAGIC_INODE) || old_inode.type != KIFS_INO_T_FILE || old_inode.extent_tree_height != 0) {
            fprintf(stderr, "kifs_cp: existing entry is not a simple file we can overwrite\n");
            kifs_close_image(&fs);
            free(src);
            return 1;
        }
        free_inode_blocks(&fs, &old_inode);
    }

    need_blocks = (uint32_t)((src_len + (KIFS_BLOCK_SIZE - 1u)) / KIFS_BLOCK_SIZE);
    if (!allocate_extents(&fs, need_blocks, extents, &extent_count)) {
        fprintf(stderr, "kifs_cp: not enough free KiFS data blocks for %s\n", root_name);
        kifs_close_image(&fs);
        free(src);
        return 1;
    }

    if (!write_file_data(&fs, src, src_len, extents, extent_count)) {
        fprintf(stderr, "kifs_cp: failed writing data blocks\n");
        kifs_close_image(&fs);
        free(src);
        return 1;
    }

    inode_zero(ino_slot);
    ino_slot->type = KIFS_INO_T_FILE;
    ino_slot->mode = 0644;
    ino_slot->link_count = 1;
    ino_slot->size_bytes = src_len;
    ino_slot->inline_extent_count = extent_count;
    for (uint16_t i = 0; i < extent_count; i++) {
        ino_slot->inline_extents[i] = extents[i];
    }

    bitmap_set_mem(fs.inode_bitmap, fs.sb.inode_bitmap_blocks, ino);

    if (!ensure_root_entry(&fs, root_name, ino, !new_inode)) {
        kifs_close_image(&fs);
        free(src);
        return 1;
    }

    if (!flush_metadata(&fs)) {
        fprintf(stderr, "kifs_cp: failed flushing KiFS metadata back to disk\n");
        kifs_close_image(&fs);
        free(src);
        return 1;
    }

    printf("kifs_cp: copied %s to /%s (inode %" PRIu32 ", %zu bytes)\n", argv[3], root_name, ino, src_len);
    kifs_close_image(&fs);
    free(src);
    return 0;
}
