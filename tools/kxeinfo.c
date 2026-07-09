#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KXE_MAGIC "KXE\0"
#define KXE_VERSION_MAJOR 0u
#define KXE_VERSION_MINOR 1u
#define KXE_HEADER_SIZE 64u
#define KXE_SECTION_SIZE 48u
#define KXE_HEADER_PAGE_SIZE 4096u
#define KXE_MAX_SECTIONS 16u

#define KXE_SECTION_TYPE_LOAD   1u
#define KXE_SECTION_TYPE_NOBITS 2u

#define KXE_SECTION_FLAG_READ  (1u << 0)
#define KXE_SECTION_FLAG_WRITE (1u << 1)
#define KXE_SECTION_FLAG_EXEC  (1u << 2)

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

static uint32_t crc32_ieee(const void* data, size_t len) {
    static uint32_t table[256];
    static int table_init = 0;
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xffffffffu;

    if (!table_init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_init = 1;
    }

    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
    }

    return crc ^ 0xffffffffu;
}

static int read_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    FILE* fp;
    long len;
    uint8_t* buf;

    *out_buf = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "kxeinfo: failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "kxeinfo: failed to measure %s\n", path);
        fclose(fp);
        return 1;
    }

    buf = (uint8_t*)malloc((size_t)len);
    if (!buf) {
        fprintf(stderr, "kxeinfo: out of memory\n");
        fclose(fp);
        return 1;
    }

    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fprintf(stderr, "kxeinfo: short read on %s\n", path);
        free(buf);
        fclose(fp);
        return 1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)len;
    return 0;
}

static void section_name(const kxe_section_t* sec, char out[9]) {
    memcpy(out, sec->name, 8);
    out[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        if (out[i] != '\0') break;
        out[i] = '\0';
    }
}

static const char* section_type_name(uint16_t type) {
    switch (type) {
        case KXE_SECTION_TYPE_LOAD:
            return "LOAD";
        case KXE_SECTION_TYPE_NOBITS:
            return "NOBITS";
        default:
            return "UNKNOWN";
    }
}

static void flags_string(uint16_t flags, char out[4]) {
    out[0] = (flags & KXE_SECTION_FLAG_READ) ? 'R' : '-';
    out[1] = (flags & KXE_SECTION_FLAG_WRITE) ? 'W' : '-';
    out[2] = (flags & KXE_SECTION_FLAG_EXEC) ? 'X' : '-';
    out[3] = '\0';
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_size, uint64_t b_start, uint64_t b_size) {
    uint64_t a_end = a_start + a_size;
    uint64_t b_end = b_start + b_size;
    if (a_end < a_start || b_end < b_start) return true;
    return a_start < b_end && b_start < a_end;
}

static int validate_kxe(const uint8_t* file, size_t file_len, bool verbose) {
    kxe_header_t hdr;
    kxe_header_t crc_hdr;
    const kxe_section_t* sections;
    bool entry_in_exec = false;

    if (file_len < KXE_HEADER_PAGE_SIZE) {
        fprintf(stderr, "kxeinfo: file is smaller than KXE header page\n");
        return 1;
    }

    memcpy(&hdr, file, sizeof(hdr));
    if (memcmp(hdr.magic, KXE_MAGIC, 4) != 0) {
        fprintf(stderr, "kxeinfo: bad magic\n");
        return 1;
    }

    if (hdr.version_major != KXE_VERSION_MAJOR || hdr.version_minor != KXE_VERSION_MINOR) {
        fprintf(stderr, "kxeinfo: unsupported version %" PRIu16 ".%" PRIu16 "\n",
                hdr.version_major,
                hdr.version_minor);
        return 1;
    }

    if (hdr.header_size != KXE_HEADER_SIZE ||
        hdr.section_entry_size != KXE_SECTION_SIZE ||
        hdr.section_count == 0 ||
        hdr.section_count > KXE_MAX_SECTIONS ||
        hdr.section_table_offset + ((uint64_t)hdr.section_count * KXE_SECTION_SIZE) > KXE_HEADER_PAGE_SIZE) {
        fprintf(stderr, "kxeinfo: invalid header layout\n");
        return 1;
    }

    if (hdr.file_size != file_len) {
        fprintf(stderr, "kxeinfo: header file_size=%" PRIu64 " actual=%zu\n", hdr.file_size, file_len);
        return 1;
    }

    crc_hdr = hdr;
    crc_hdr.header_crc32 = 0;
    if (crc32_ieee(&crc_hdr, sizeof(crc_hdr)) != hdr.header_crc32) {
        fprintf(stderr, "kxeinfo: header CRC mismatch\n");
        return 1;
    }

    sections = (const kxe_section_t*)(file + hdr.section_table_offset);
    for (uint16_t i = 0; i < hdr.section_count; i++) {
        const kxe_section_t* sec = &sections[i];
        uint64_t vend = sec->vaddr + sec->vsize;

        if (sec->vsize == 0 || vend < sec->vaddr) {
            fprintf(stderr, "kxeinfo: section %" PRIu16 " has invalid virtual range\n", i);
            return 1;
        }

        if (sec->type != KXE_SECTION_TYPE_LOAD && sec->type != KXE_SECTION_TYPE_NOBITS) {
            fprintf(stderr, "kxeinfo: section %" PRIu16 " has unknown type\n", i);
            return 1;
        }

        if ((sec->flags & (KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) ==
            (KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) {
            fprintf(stderr, "kxeinfo: section %" PRIu16 " is W+X\n", i);
            return 1;
        }

        if (sec->type == KXE_SECTION_TYPE_LOAD) {
            if (sec->file_size == 0 || sec->file_size > sec->vsize ||
                sec->file_offset + sec->file_size < sec->file_offset ||
                sec->file_offset + sec->file_size > file_len) {
                fprintf(stderr, "kxeinfo: section %" PRIu16 " has invalid file range\n", i);
                return 1;
            }
        } else if (sec->file_size != 0 || sec->file_offset != 0) {
            fprintf(stderr, "kxeinfo: NOBITS section %" PRIu16 " has file payload\n", i);
            return 1;
        }

        if (hdr.entry_vaddr >= sec->vaddr && hdr.entry_vaddr < vend &&
            (sec->flags & KXE_SECTION_FLAG_EXEC) != 0) {
            entry_in_exec = true;
        }

        for (uint16_t j = (uint16_t)(i + 1u); j < hdr.section_count; j++) {
            if (ranges_overlap(sec->vaddr, sec->vsize, sections[j].vaddr, sections[j].vsize)) {
                fprintf(stderr, "kxeinfo: sections %" PRIu16 " and %" PRIu16 " overlap\n", i, j);
                return 1;
            }
        }
    }

    if (!entry_in_exec) {
        fprintf(stderr, "kxeinfo: entry point is not inside an executable section\n");
        return 1;
    }

    if (verbose) {
        printf("KXE %" PRIu16 ".%" PRIu16 " abi=%" PRIu16 " sections=%" PRIu16 "\n",
               hdr.version_major,
               hdr.version_minor,
               hdr.abi_version,
               hdr.section_count);
        printf("entry=0x%016" PRIx64 " image=[0x%016" PRIx64 ",0x%016" PRIx64 ") file=%" PRIu64 " crc=0x%08" PRIx32 "\n",
               hdr.entry_vaddr,
               hdr.image_min_vaddr,
               hdr.image_max_vaddr,
               hdr.file_size,
               hdr.header_crc32);
        printf("idx name     type   flg vaddr              vsize      file_off   file_size\n");
        for (uint16_t i = 0; i < hdr.section_count; i++) {
            char name[9];
            char flg[4];
            section_name(&sections[i], name);
            flags_string(sections[i].flags, flg);
            printf("%3" PRIu16 " %-8s %-6s %-3s 0x%016" PRIx64 " 0x%08" PRIx64 " 0x%08" PRIx64 " 0x%08" PRIx64 "\n",
                   i,
                   name,
                   section_type_name(sections[i].type),
                   flg,
                   sections[i].vaddr,
                   sections[i].vsize,
                   sections[i].file_offset,
                   sections[i].file_size);
        }
    }

    return 0;
}

int main(int argc, char** argv) {
    uint8_t* file = NULL;
    size_t file_len = 0;
    int rc;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.kxe>\n", argv[0]);
        return 1;
    }

    if (read_file(argv[1], &file, &file_len) != 0) {
        return 1;
    }

    rc = validate_kxe(file, file_len, true);
    free(file);
    return rc;
}
