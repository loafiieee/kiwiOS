#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "fs/kifs/kifs_disk.h"

#define SECTOR_SIZE 512u
#define KIFS_FS_MAJOR 0u
#define KIFS_FS_MINOR 1u
#define KIFS_BITMAP_PAYLOAD_BYTES (KIFS_BLOCK_SIZE - (uint32_t)sizeof(kifs_shdr_t))
#define KIFS_BITMAP_BITS_PER_BLOCK (KIFS_BITMAP_PAYLOAD_BYTES * 8u)

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name_utf16le[36];
} gpt_entry_t;

static uint32_t g_crc32_table[256];
static bool g_crc32_table_init = false;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (uint32_t j = 0; j < 8u; j++) {
            if ((c & 1u) != 0u) {
                c = 0xEDB88320u ^ (c >> 1u);
            } else {
                c >>= 1u;
            }
        }
        g_crc32_table[i] = c;
    }
    g_crc32_table_init = true;
}

static uint32_t crc32_ieee(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (!g_crc32_table_init) {
        crc32_init_table();
    }

    for (size_t i = 0; i < len; i++) {
        crc = g_crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8u);
    }

    return crc ^ 0xFFFFFFFFu;
}

static int read_at(FILE* fp, uint64_t off, void* buf, size_t len) {
    if (!fp || !buf) {
        return -1;
    }
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0) {
        return -1;
    }
    return (fread(buf, 1, len, fp) == len) ? 0 : -1;
}

static int write_at(FILE* fp, uint64_t off, const void* buf, size_t len) {
    if (!fp || !buf) {
        return -1;
    }
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buf, 1, len, fp) != len) {
        return -1;
    }
    return 0;
}

static bool parse_u32_strict(const char* s, uint32_t* out) {
    uint64_t value = 0;

    if (!s || !*s || !out) {
        return false;
    }

    while (*s >= '0' && *s <= '9') {
        value = (value * 10u) + (uint64_t)(*s - '0');
        if (value > 0xffffffffu) {
            return false;
        }
        s++;
    }

    if (*s != '\0') {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static bool gpt_entry_is_empty(const gpt_entry_t* ent) {
    for (size_t i = 0; i < sizeof(ent->type_guid); i++) {
        if (ent->type_guid[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool gpt_get_partition(FILE* fp,
                              uint32_t part_number,
                              uint64_t* out_first_lba,
                              uint64_t* out_last_lba) {
    gpt_header_t hdr;
    gpt_entry_t ent;
    uint64_t ent_off = 0;

    if (!fp || !out_first_lba || !out_last_lba || part_number == 0u) {
        fprintf(stderr, "mkfs_kifs: partition numbers are 1-based\n");
        return false;
    }

    if (read_at(fp, SECTOR_SIZE, &hdr, sizeof(hdr)) != 0) {
        fprintf(stderr, "mkfs_kifs: failed to read GPT header\n");
        return false;
    }

    if (memcmp(hdr.signature, "EFI PART", 8u) != 0) {
        fprintf(stderr, "mkfs_kifs: disk image does not contain a GPT header\n");
        return false;
    }

    if (part_number > hdr.num_partition_entries ||
        hdr.sizeof_partition_entry < sizeof(gpt_entry_t)) {
        if (part_number > hdr.num_partition_entries) {
            fprintf(stderr, "mkfs_kifs: partition %" PRIu32 " is out of range\n", part_number);
        } else {
            fprintf(stderr, "mkfs_kifs: unsupported GPT entry size\n");
        }
        return false;
    }

    ent_off = (hdr.partition_entry_lba * SECTOR_SIZE) +
              ((uint64_t)(part_number - 1u) * hdr.sizeof_partition_entry);
    if (read_at(fp, ent_off, &ent, sizeof(ent)) != 0) {
        fprintf(stderr, "mkfs_kifs: failed to read GPT entry %" PRIu32 "\n", part_number);
        return false;
    }

    if (gpt_entry_is_empty(&ent) || ent.first_lba == 0u || ent.last_lba < ent.first_lba) {
        fprintf(stderr, "mkfs_kifs: GPT entry %" PRIu32 " is empty\n", part_number);
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
    if (sb->total_blocks == 0u || sb->total_blocks > dev_blocks) {
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

    if (sb->block_bitmap_start < 2u || bb_end > sb->usable_blocks) {
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

    return sb->root_ino == 1u && sb->inode_count > 1u;
}

static bool read_block(FILE* fp, uint64_t part_offset, uint32_t blkno, uint8_t out[KIFS_BLOCK_SIZE]) {
    return read_at(fp, part_offset + ((uint64_t)blkno * KIFS_BLOCK_SIZE), out, KIFS_BLOCK_SIZE) == 0;
}

static bool write_block(FILE* fp, uint64_t part_offset, uint32_t blkno, const uint8_t in[KIFS_BLOCK_SIZE]) {
    return write_at(fp, part_offset + ((uint64_t)blkno * KIFS_BLOCK_SIZE), in, KIFS_BLOCK_SIZE) == 0;
}

static bool validate_superblock_at(FILE* fp,
                                   uint64_t part_offset,
                                   uint32_t dev_blocks,
                                   uint32_t blkno,
                                   kifs_superblock_t* out) {
    uint8_t buf[KIFS_BLOCK_SIZE];
    if (!read_block(fp, part_offset, blkno, buf)) {
        return false;
    }
    if (!validate_shdr(buf, KIFS_MAGIC_SUPER, KIFS_BTYPE_SUPER, true)) {
        return false;
    }
    if (((const kifs_superblock_t*)buf)->sb_blockno != blkno) {
        return false;
    }
    if (!sb_layout_sane((const kifs_superblock_t*)buf, dev_blocks)) {
        return false;
    }
    if (out) {
        memcpy(out, buf, sizeof(*out));
    }
    return true;
}

static bool partition_has_valid_kifs(FILE* fp, uint64_t part_offset, uint32_t part_blocks) {
    kifs_superblock_t a;
    kifs_superblock_t b;
    return validate_superblock_at(fp, part_offset, part_blocks, 0u, &a) ||
           validate_superblock_at(fp, part_offset, part_blocks, 1u, &b);
}

static void bitmap_set(uint8_t* bm_payload, uint32_t bit) {
    bm_payload[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static bool bitmap_set_in_blocks(uint8_t* bm_mem, uint32_t bm_blocks, uint32_t bit_index) {
    uint32_t bi = bit_index / KIFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = bit_index % KIFS_BITMAP_BITS_PER_BLOCK;
    uint8_t* payload = NULL;

    if (bi >= bm_blocks) {
        return false;
    }

    payload = bm_mem + (bi * KIFS_BLOCK_SIZE) + (uint32_t)sizeof(kifs_shdr_t);
    bitmap_set(payload, bit);
    return true;
}

static void init_bitmap_block(uint8_t* out_blk, const char* magic, uint16_t btype) {
    kifs_shdr_t* h = NULL;

    memset(out_blk, 0, KIFS_BLOCK_SIZE);
    h = (kifs_shdr_t*)out_blk;
    memcpy(h->magic, magic, 4u);
    h->version = KIFS_STRUCT_VERSION;
    h->block_type = btype;
    h->header_bytes = (uint16_t)sizeof(kifs_shdr_t);
    h->payload_bytes = (uint16_t)KIFS_BITMAP_PAYLOAD_BYTES;
    h->flags = 0;
    h->checksum = 0;
    h->checksum = crc32_ieee(out_blk, KIFS_BLOCK_SIZE);
}

static void structured_rechecksum(uint8_t* blk) {
    kifs_shdr_t* h = (kifs_shdr_t*)blk;
    h->checksum = 0;
    h->checksum = crc32_ieee(blk, KIFS_BLOCK_SIZE);
}

static void init_dir_block(uint8_t* out_blk, uint32_t parent_hint) {
    kifs_shdr_t* h = NULL;

    memset(out_blk, 0, KIFS_BLOCK_SIZE);
    h = (kifs_shdr_t*)out_blk;
    memcpy(h->magic, KIFS_MAGIC_DIR, 4u);
    h->version = KIFS_STRUCT_VERSION;
    h->block_type = KIFS_BTYPE_DIR;
    h->header_bytes = (uint16_t)(sizeof(kifs_shdr_t) + 4u);
    h->payload_bytes = (uint16_t)(KIFS_BLOCK_SIZE - (uint32_t)h->header_bytes);
    h->flags = 0;
    *(uint32_t*)(out_blk + sizeof(kifs_shdr_t)) = parent_hint;
    h->checksum = 0;
    h->checksum = crc32_ieee(out_blk, KIFS_BLOCK_SIZE);
}

static uint16_t dir_rec_len(uint8_t name_len) {
    uint32_t n = 8u + (uint32_t)name_len;
    return (uint16_t)((n + 7u) & ~7u);
}

static void dir_write_ent(uint8_t* blk,
                          uint32_t off,
                          uint32_t ino,
                          const char* name,
                          uint8_t ftype,
                          uint16_t rec_len) {
    uint8_t* p = blk + off;
    uint8_t namelen = (uint8_t)strlen(name);

    *(uint32_t*)(p + 0u) = ino;
    *(uint16_t*)(p + 4u) = rec_len;
    *(uint8_t*)(p + 6u) = namelen;
    *(uint8_t*)(p + 7u) = ftype;
    memcpy(p + 8u, name, namelen);
}

static void inode_zero(kifs_inode_t* ino) {
    memset(ino, 0, sizeof(*ino));
    memcpy(ino->magic, KIFS_MAGIC_INODE, 4u);
    ino->version = 1u;
}

static bool mkfs_partition(FILE* fp, uint64_t part_offset, uint32_t total_blocks, uint32_t inode_count) {
    static const char* base_dir_names[] = { "bin", "dev", "mnt", "home", "tmp" };
    uint32_t journal_blocks = 0;
    uint32_t usable_blocks = 0;

    if (!fp || total_blocks < 64u) {
        return false;
    }

    if (inode_count == 0u) {
        inode_count = 1024u;
    }
    if (inode_count < 16u) {
        inode_count = 16u;
    }

    journal_blocks = total_blocks / 100u;
    usable_blocks = total_blocks - journal_blocks;

    while (1) {
        uint32_t bb_blocks = (usable_blocks + (KIFS_BITMAP_BITS_PER_BLOCK - 1u)) / KIFS_BITMAP_BITS_PER_BLOCK;
        uint32_t ib_blocks = (inode_count + (KIFS_BITMAP_BITS_PER_BLOCK - 1u)) / KIFS_BITMAP_BITS_PER_BLOCK;
        uint32_t it_blocks = (inode_count + 15u) / 16u;
        uint32_t data_start = 2u + bb_blocks + ib_blocks + it_blocks;
        uint32_t root_dir_blk = 0;
        uint32_t dir_blks[5];
        uint8_t* bb_mem = NULL;
        uint8_t* ib_mem = NULL;
        uint8_t* it_mem = NULL;
        uint8_t* data_mem = NULL;
        kifs_superblock_t sb;
        kifs_superblock_t s0;
        kifs_superblock_t s1;

        if (data_start + 6u > usable_blocks) {
            if (inode_count <= 16u) {
                return false;
            }
            inode_count /= 2u;
            continue;
        }

        memset(&sb, 0, sizeof(sb));
        memcpy(sb.h.magic, KIFS_MAGIC_SUPER, 4u);
        sb.h.version = KIFS_STRUCT_VERSION;
        sb.h.block_type = KIFS_BTYPE_SUPER;
        sb.h.header_bytes = (uint16_t)sizeof(kifs_shdr_t);
        sb.h.payload_bytes = (uint16_t)(KIFS_BLOCK_SIZE - (uint32_t)sizeof(kifs_shdr_t));
        sb.block_size = KIFS_BLOCK_SIZE;
        sb.fs_major = KIFS_FS_MAJOR;
        sb.fs_minor = KIFS_FS_MINOR;
        sb.total_blocks = total_blocks;
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
        sb.root_ino = 1u;
        sb.orphan_ino = 2u;
        sb.features_incompat = KIFS_FEAT_INCOMPAT_META_CRC | KIFS_FEAT_INCOMPAT_ORPHAN_FILE;
        sb.sb_seq = 1u;

        root_dir_blk = data_start;
        for (uint32_t i = 0; i < 5u; i++) {
            dir_blks[i] = data_start + 1u + i;
        }

        bb_mem = (uint8_t*)calloc(bb_blocks, KIFS_BLOCK_SIZE);
        ib_mem = (uint8_t*)calloc(ib_blocks, KIFS_BLOCK_SIZE);
        it_mem = (uint8_t*)calloc(it_blocks, KIFS_BLOCK_SIZE);
        data_mem = (uint8_t*)calloc(6u, KIFS_BLOCK_SIZE);
        if (!bb_mem || !ib_mem || !it_mem || !data_mem) {
            free(bb_mem);
            free(ib_mem);
            free(it_mem);
            free(data_mem);
            return false;
        }

        for (uint32_t i = 0; i < bb_blocks; i++) {
            init_bitmap_block(bb_mem + (i * KIFS_BLOCK_SIZE), KIFS_MAGIC_BMB, KIFS_BTYPE_BMB);
        }
        bitmap_set_in_blocks(bb_mem, bb_blocks, 0u);
        bitmap_set_in_blocks(bb_mem, bb_blocks, 1u);
        for (uint32_t i = 0; i < bb_blocks; i++) {
            bitmap_set_in_blocks(bb_mem, bb_blocks, sb.block_bitmap_start + i);
        }
        for (uint32_t i = 0; i < ib_blocks; i++) {
            bitmap_set_in_blocks(bb_mem, bb_blocks, sb.inode_bitmap_start + i);
        }
        for (uint32_t i = 0; i < it_blocks; i++) {
            bitmap_set_in_blocks(bb_mem, bb_blocks, sb.inode_table_start + i);
        }
        bitmap_set_in_blocks(bb_mem, bb_blocks, root_dir_blk);
        for (uint32_t i = 0; i < 5u; i++) {
            bitmap_set_in_blocks(bb_mem, bb_blocks, dir_blks[i]);
        }
        for (uint32_t i = 0; i < bb_blocks; i++) {
            structured_rechecksum(bb_mem + (i * KIFS_BLOCK_SIZE));
        }

        for (uint32_t i = 0; i < ib_blocks; i++) {
            init_bitmap_block(ib_mem + (i * KIFS_BLOCK_SIZE), KIFS_MAGIC_IMB, KIFS_BTYPE_IMB);
        }
        bitmap_set_in_blocks(ib_mem, ib_blocks, 0u);
        for (uint32_t ino = 1u; ino <= 7u; ino++) {
            bitmap_set_in_blocks(ib_mem, ib_blocks, ino);
        }
        for (uint32_t i = 0; i < ib_blocks; i++) {
            structured_rechecksum(ib_mem + (i * KIFS_BLOCK_SIZE));
        }

        {
            enum {
                KIFS_MKFS_ROOT_INO = 1,
                KIFS_MKFS_ORPHAN_INO = 2,
                KIFS_MKFS_BIN_INO = 3,
                KIFS_MKFS_DEV_INO = 4,
                KIFS_MKFS_MNT_INO = 5,
                KIFS_MKFS_HOME_INO = 6,
                KIFS_MKFS_TMP_INO = 7,
            };
            kifs_inode_t root;
            kifs_inode_t orphan;
            kifs_inode_t dirs[5];

            inode_zero(&root);
            root.type = KIFS_INO_T_DIR;
            root.mode = 0755u;
            root.link_count = 7u;
            root.size_bytes = KIFS_BLOCK_SIZE;
            root.inline_extent_count = 1u;
            root.inline_extents[0].disk_block_start = root_dir_blk;
            root.inline_extents[0].block_count = 1u;

            inode_zero(&orphan);
            orphan.type = KIFS_INO_T_FILE;
            orphan.link_count = 1u;

            for (uint32_t i = 0; i < 5u; i++) {
                inode_zero(&dirs[i]);
                dirs[i].type = KIFS_INO_T_DIR;
                dirs[i].mode = 0755u;
                dirs[i].link_count = 2u;
                dirs[i].size_bytes = KIFS_BLOCK_SIZE;
                dirs[i].inline_extent_count = 1u;
                dirs[i].inline_extents[0].disk_block_start = dir_blks[i];
                dirs[i].inline_extents[0].block_count = 1u;
            }

            memcpy(it_mem + (KIFS_MKFS_ROOT_INO * 256u), &root, sizeof(root));
            memcpy(it_mem + (KIFS_MKFS_ORPHAN_INO * 256u), &orphan, sizeof(orphan));
            for (uint32_t i = 0; i < 5u; i++) {
                memcpy(it_mem + ((KIFS_MKFS_BIN_INO + i) * 256u), &dirs[i], sizeof(dirs[i]));
            }

            {
                uint8_t* dirblk = data_mem;
                uint32_t doff = 0u;

                init_dir_block(dirblk, KIFS_MKFS_ROOT_INO);
                doff = ((kifs_shdr_t*)dirblk)->header_bytes;
                dir_write_ent(dirblk, doff, KIFS_MKFS_ROOT_INO, ".", 2u, dir_rec_len(1u));
                doff += dir_rec_len(1u);
                dir_write_ent(dirblk, doff, KIFS_MKFS_ROOT_INO, "..", 2u, dir_rec_len(2u));
                doff += dir_rec_len(2u);
                for (uint32_t i = 0; i < 5u; i++) {
                    uint16_t rec = dir_rec_len((uint8_t)strlen(base_dir_names[i]));
                    dir_write_ent(dirblk, doff, KIFS_MKFS_BIN_INO + i, base_dir_names[i], 2u, rec);
                    doff += rec;
                }
                structured_rechecksum(dirblk);
            }

            for (uint32_t i = 0; i < 5u; i++) {
                uint8_t* dirblk = data_mem + ((i + 1u) * KIFS_BLOCK_SIZE);
                uint32_t doff = 0u;
                uint16_t r1 = 0u;
                uint16_t r2 = 0u;

                init_dir_block(dirblk, KIFS_MKFS_ROOT_INO);
                doff = ((kifs_shdr_t*)dirblk)->header_bytes;
                r1 = dir_rec_len(1u);
                dir_write_ent(dirblk, doff, KIFS_MKFS_BIN_INO + i, ".", 2u, r1);
                doff += r1;
                r2 = (uint16_t)((KIFS_BLOCK_SIZE - (uint32_t)((kifs_shdr_t*)dirblk)->header_bytes) - doff);
                r2 = (uint16_t)(r2 & ~7u);
                if (r2 < dir_rec_len(2u)) {
                    r2 = dir_rec_len(2u);
                }
                dir_write_ent(dirblk, doff, KIFS_MKFS_ROOT_INO, "..", 2u, r2);
                structured_rechecksum(dirblk);
            }
        }

        s0 = sb;
        s0.sb_blockno = 0u;
        s0.h.checksum = 0u;
        s0.h.checksum = crc32_ieee(&s0, KIFS_BLOCK_SIZE);

        s1 = sb;
        s1.sb_blockno = 1u;
        s1.h.checksum = 0u;
        s1.h.checksum = crc32_ieee(&s1, KIFS_BLOCK_SIZE);

        if (!write_block(fp, part_offset, 0u, (const uint8_t*)&s0) ||
            !write_block(fp, part_offset, 1u, (const uint8_t*)&s1)) {
            free(bb_mem);
            free(ib_mem);
            free(it_mem);
            free(data_mem);
            return false;
        }

        for (uint32_t i = 0; i < bb_blocks; i++) {
            if (!write_block(fp, part_offset, sb.block_bitmap_start + i, bb_mem + (i * KIFS_BLOCK_SIZE))) {
                free(bb_mem);
                free(ib_mem);
                free(it_mem);
                free(data_mem);
                return false;
            }
        }
        for (uint32_t i = 0; i < ib_blocks; i++) {
            if (!write_block(fp, part_offset, sb.inode_bitmap_start + i, ib_mem + (i * KIFS_BLOCK_SIZE))) {
                free(bb_mem);
                free(ib_mem);
                free(it_mem);
                free(data_mem);
                return false;
            }
        }
        for (uint32_t i = 0; i < it_blocks; i++) {
            if (!write_block(fp, part_offset, sb.inode_table_start + i, it_mem + (i * KIFS_BLOCK_SIZE))) {
                free(bb_mem);
                free(ib_mem);
                free(it_mem);
                free(data_mem);
                return false;
            }
        }

        if (!write_block(fp, part_offset, root_dir_blk, data_mem)) {
            free(bb_mem);
            free(ib_mem);
            free(it_mem);
            free(data_mem);
            return false;
        }
        for (uint32_t i = 0; i < 5u; i++) {
            if (!write_block(fp, part_offset, dir_blks[i], data_mem + ((i + 1u) * KIFS_BLOCK_SIZE))) {
                free(bb_mem);
                free(ib_mem);
                free(it_mem);
                free(data_mem);
                return false;
            }
        }

        free(bb_mem);
        free(ib_mem);
        free(it_mem);
        free(data_mem);

        if (fflush(fp) != 0) {
            return false;
        }
        return true;
    }
}

static void usage(const char* argv0) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s --check <disk.img> <partition_number>\n", argv0);
    fprintf(stderr, "  %s <disk.img> <partition_number> [inode_count]\n", argv0);
}

int main(int argc, char** argv) {
    bool check_only = false;
    const char* disk_path = NULL;
    FILE* fp = NULL;
    uint32_t part_number = 0;
    uint32_t inode_count = 1024u;
    uint64_t first_lba = 0;
    uint64_t last_lba = 0;
    uint64_t part_offset = 0;
    uint32_t part_blocks = 0;
    bool ok = false;

    if (argc >= 2 && strcmp(argv[1], "--check") == 0) {
        check_only = true;
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }
        disk_path = argv[2];
        if (!parse_u32_strict(argv[3], &part_number) || part_number == 0u) {
            usage(argv[0]);
            return 1;
        }
    } else {
        if (argc != 3 && argc != 4) {
            usage(argv[0]);
            return 1;
        }
        disk_path = argv[1];
        if (!parse_u32_strict(argv[2], &part_number) || part_number == 0u) {
            usage(argv[0]);
            return 1;
        }
        if (argc == 4 && !parse_u32_strict(argv[3], &inode_count)) {
            usage(argv[0]);
            return 1;
        }
    }

    fp = fopen(disk_path, check_only ? "rb" : "rb+");
    if (!fp) {
        fprintf(stderr, "mkfs_kifs: failed to open %s: %s\n", disk_path, strerror(errno));
        return 1;
    }

    if (!gpt_get_partition(fp, part_number, &first_lba, &last_lba)) {
        fprintf(stderr, "mkfs_kifs: failed to locate GPT partition %" PRIu32 "\n", part_number);
        fclose(fp);
        return 1;
    }

    part_offset = first_lba * SECTOR_SIZE;
    part_blocks = (uint32_t)((((last_lba - first_lba) + 1u) * SECTOR_SIZE) / KIFS_BLOCK_SIZE);
    if (part_blocks < 64u) {
        fprintf(stderr, "mkfs_kifs: partition %" PRIu32 " is too small for KiFS\n", part_number);
        fclose(fp);
        return 1;
    }

    if (check_only) {
        ok = partition_has_valid_kifs(fp, part_offset, part_blocks);
        fclose(fp);
        return ok ? 0 : 1;
    }

    ok = mkfs_partition(fp, part_offset, part_blocks, inode_count);
    if (!ok) {
        fprintf(stderr, "mkfs_kifs: failed to format partition %" PRIu32 "\n", part_number);
        fclose(fp);
        return 1;
    }

    fclose(fp);
    printf("mkfs_kifs: formatted %s partition %" PRIu32 " (%" PRIu32 " KiFS blocks)\n",
           disk_path,
           part_number,
           part_blocks);
    return 0;
}
