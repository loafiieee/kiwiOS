#pragma once
#include <stdint.h>

#define KIFS_BLOCK_SIZE 4096u

#define KIFS_MAGIC_SUPER "KIFS"
#define KIFS_MAGIC_INODE "KINO"
#define KIFS_MAGIC_DIR   "KDIR"
#define KIFS_MAGIC_EXT   "KEXT"
#define KIFS_MAGIC_BMB   "KBMB"
#define KIFS_MAGIC_IMB   "KIMB"

#define KIFS_STRUCT_VERSION 1u

// Structured block types (u16)
enum {
    KIFS_BTYPE_SUPER = 1,
    KIFS_BTYPE_BMB   = 2,
    KIFS_BTYPE_IMB   = 3,
    KIFS_BTYPE_DIR   = 4,
    KIFS_BTYPE_EXT   = 5,
};

// Inode types (u16)
enum {
    KIFS_INO_T_INVALID = 0,
    KIFS_INO_T_FILE    = 1,
    KIFS_INO_T_DIR     = 2,
};

// Feature flags (v0.1)
// Unknown incompatible bits MUST refuse mount.
#define KIFS_FEAT_INCOMPAT_META_CRC   (1u << 0) // directory blocks + extent nodes require CRC32
#define KIFS_FEAT_INCOMPAT_ORPHAN_FILE (1u << 1) // inode 2 reserved for orphan file

// Structured header (present at start of every metadata 4 KiB block except inode-table blocks).
typedef struct __attribute__((packed)) {
    char     magic[4];      // e.g. "KIFS", "KDIR", "KEXT"
    uint16_t version;       // KIFS_STRUCT_VERSION
    uint16_t block_type;    // KIFS_BTYPE_*
    uint16_t header_bytes;  // bytes before payload
    uint16_t payload_bytes; // maximum parsable payload bytes
    uint32_t flags;
    uint32_t checksum;      // CRC32 of entire 4 KiB block with this field set to 0 (if enabled/required)
} kifs_shdr_t;

// Superblock (exactly 4096 bytes)
typedef struct __attribute__((packed)) {
    kifs_shdr_t h;

    uint16_t fs_major;
    uint16_t fs_minor;

    uint32_t block_size;     // must be 4096
    uint32_t sb_blockno;     // 0 or 1
    uint32_t dirty;          // 0 clean, 1 dirty

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

    uint8_t  pad[3964];
} kifs_superblock_t;

_Static_assert(sizeof(kifs_superblock_t) == KIFS_BLOCK_SIZE, "kifs_superblock_t must be 4096 bytes");

// Extent record (12 bytes)
typedef struct __attribute__((packed)) {
    uint32_t file_block_start; // file block index (4 KiB units)
    uint32_t disk_block_start; // absolute filesystem block number
    uint32_t block_count;      // length in blocks
} kifs_extent_t;

// Inode record (256 bytes)
typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t version;
    uint16_t type;          // KIFS_INO_T_*

    uint32_t mode;          // basic rwx bits
    uint32_t uid;
    uint32_t gid;

    uint32_t link_count;

    uint64_t size_bytes;
    uint64_t mtime;
    uint64_t ctime;

    uint16_t inline_extent_count; // <= 8
    uint16_t extent_tree_height;  // 0 => inline-only
    uint32_t extent_tree_root;    // block number (0 if none)

    kifs_extent_t inline_extents[8];

    uint32_t inode_checksum; // optional (unused in v0.1)
    uint8_t  reserved[100];
} kifs_inode_t;

_Static_assert(sizeof(kifs_inode_t) == 256, "kifs_inode_t must be 256 bytes");

// Directory block header: structured header + parent hint.
// Entries begin at offset h.header_bytes.
typedef struct __attribute__((packed)) {
    kifs_shdr_t h;
    uint32_t parent_ino_hint;
    uint8_t  pad[4072]; // payload region follows (dirents), but we treat block as raw bytes.
} kifs_dir_block_t;

// Extent tree node header: structured header + node fields.
typedef struct __attribute__((packed)) {
    kifs_shdr_t h;
    uint16_t node_type;   // 0 leaf, 1 internal
    uint16_t height;      // 0 leaf
    uint16_t entry_count;
    uint16_t reserved;
    // payload entries follow
} kifs_ext_node_hdr_t;

// Internal node entry: (key, child_blockno)
typedef struct __attribute__((packed)) {
    uint32_t first_file_block;
    uint32_t child_blockno;
} kifs_ext_internal_ent_t;

