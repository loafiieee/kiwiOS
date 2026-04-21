#ifndef CORE_KXE_H
#define CORE_KXE_H

#include <stdbool.h>
#include <stdint.h>
#include "core/process.h"

#define KXE_MAGIC               "KXE\0"
#define KXE_VERSION_MAJOR       0u
#define KXE_VERSION_MINOR       1u
#define KXE_HEADER_SIZE         64u
#define KXE_SECTION_SIZE        48u
#define KXE_MAX_SECTIONS        16u
#define KXE_HEADER_PAGE_SIZE    4096u
#define KXE_USER_STACK_TOP      0x0000000000800000ull
#define KXE_USER_STACK_PAGES    16u
#define KXE_USER_MIN_VADDR      0x0000000000001000ull
#define KXE_USER_MAX_VADDR      0x0000800000000000ull

enum {
    KXE_FLAG_NONE = 0u,
};

enum {
    KXE_SECTION_TYPE_LOAD   = 1u,
    KXE_SECTION_TYPE_NOBITS = 2u,
};

enum {
    KXE_SECTION_FLAG_READ  = 1u << 0,
    KXE_SECTION_FLAG_WRITE = 1u << 1,
    KXE_SECTION_FLAG_EXEC  = 1u << 2,
};

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t flags;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint16_t abi_version;
    uint32_t header_crc32;
    uint64_t entry_vaddr;
    uint64_t section_table_offset;
    uint64_t image_min_vaddr;
    uint64_t image_max_vaddr;
    uint64_t file_size;
} kxe_header_t;

typedef struct __attribute__((packed)) {
    char name[8];
    uint16_t type;
    uint16_t flags;
    uint32_t reserved;
    uint64_t vaddr;
    uint64_t vsize;
    uint64_t file_offset;
    uint64_t file_size;
} kxe_section_t;

typedef struct {
    kxe_header_t header;
    kxe_section_t sections[KXE_MAX_SECTIONS];
} kxe_image_t;

_Static_assert(sizeof(kxe_header_t) == KXE_HEADER_SIZE, "kxe_header_t size must be 64 bytes");
_Static_assert(sizeof(kxe_section_t) == KXE_SECTION_SIZE, "kxe_section_t size must be 48 bytes");

bool kxe_validate(const uint8_t* header_page, uint64_t actual_file_size, kxe_image_t* out_image);
process_t* kxe_load(const char* path);

#endif // CORE_KXE_H
