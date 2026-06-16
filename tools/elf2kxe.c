#include <elf.h>
#include <errno.h>
#include <inttypes.h>
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
    kxe_section_t sec;
    uint64_t input_offset;
} section_plan_t;

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

static uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + (align - 1u)) & ~(align - 1u);
}

static void set_name(char out[8], const char* name) {
    size_t len = 0;
    memset(out, 0, 8);
    if (!name) {
        return;
    }
    while (name[len] && len < 7) {
        out[len] = name[len];
        len++;
    }
}

static const char* section_name_from_flags(uint32_t flags, uint64_t file_size) {
    if (flags & KXE_SECTION_FLAG_EXEC) {
        return ".text";
    }
    if (flags & KXE_SECTION_FLAG_WRITE) {
        return file_size == 0 ? ".bss" : ".data";
    }
    return ".rodata";
}

static uint16_t kxe_flags_from_elf(uint32_t pflags) {
    uint16_t flags = 0;
    if (pflags & PF_R) {
        flags |= KXE_SECTION_FLAG_READ;
    }
    if (pflags & PF_W) {
        flags |= KXE_SECTION_FLAG_WRITE;
    }
    if (pflags & PF_X) {
        flags |= KXE_SECTION_FLAG_EXEC;
    }
    return flags;
}

static int read_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    FILE* fp = NULL;
    long file_len = 0;
    uint8_t* buf = NULL;

    *out_buf = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "elf2kxe: failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "elf2kxe: failed to seek %s\n", path);
        fclose(fp);
        return 1;
    }

    file_len = ftell(fp);
    if (file_len < 0) {
        fprintf(stderr, "elf2kxe: failed to measure %s\n", path);
        fclose(fp);
        return 1;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "elf2kxe: failed to rewind %s\n", path);
        fclose(fp);
        return 1;
    }

    buf = (uint8_t*)malloc((size_t)file_len);
    if (!buf) {
        fprintf(stderr, "elf2kxe: out of memory reading %s\n", path);
        fclose(fp);
        return 1;
    }

    if (file_len > 0 && fread(buf, 1, (size_t)file_len, fp) != (size_t)file_len) {
        fprintf(stderr, "elf2kxe: short read on %s\n", path);
        free(buf);
        fclose(fp);
        return 1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)file_len;
    return 0;
}

static int write_padding(FILE* fp, uint64_t bytes) {
    static const uint8_t zero[16];
    while (bytes > 0) {
        size_t chunk = bytes > sizeof(zero) ? sizeof(zero) : (size_t)bytes;
        if (fwrite(zero, 1, chunk, fp) != chunk) {
            return 1;
        }
        bytes -= chunk;
    }
    return 0;
}

int main(int argc, char** argv) {
    uint8_t* elf = NULL;
    size_t elf_len = 0;
    const Elf64_Ehdr* eh = NULL;
    const Elf64_Phdr* ph = NULL;
    section_plan_t plans[KXE_MAX_SECTIONS];
    uint16_t section_count = 0;
    uint64_t image_min = UINT64_MAX;
    uint64_t image_max = 0;
    uint64_t output_cursor = KXE_HEADER_PAGE_SIZE;
    uint8_t header_page[KXE_HEADER_PAGE_SIZE];
    kxe_header_t hdr;
    FILE* out = NULL;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.elf> <output.kxe>\n", argv[0]);
        return 1;
    }

    if (read_file(argv[1], &elf, &elf_len) != 0) {
        return 1;
    }

    if (elf_len < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "elf2kxe: %s is too small to be ELF64\n", argv[1]);
        free(elf);
        return 1;
    }

    eh = (const Elf64_Ehdr*)elf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_machine != EM_X86_64 ||
        eh->e_type != ET_EXEC) {
        fprintf(stderr, "elf2kxe: %s is not a supported ELF64 x86-64 ET_EXEC image\n", argv[1]);
        free(elf);
        return 1;
    }

    if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf64_Phdr)) {
        fprintf(stderr, "elf2kxe: %s has invalid program headers\n", argv[1]);
        free(elf);
        return 1;
    }

    if ((uint64_t)eh->e_phoff + ((uint64_t)eh->e_phnum * sizeof(Elf64_Phdr)) > elf_len) {
        fprintf(stderr, "elf2kxe: %s has truncated program headers\n", argv[1]);
        free(elf);
        return 1;
    }

    ph = (const Elf64_Phdr*)(elf + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* cur = &ph[i];
        uint64_t seg_end = 0;
        uint16_t flags = 0;

        if (cur->p_type != PT_LOAD || cur->p_memsz == 0) {
            continue;
        }

        if (section_count >= KXE_MAX_SECTIONS) {
            fprintf(stderr, "elf2kxe: too many PT_LOAD segments\n");
            free(elf);
            return 1;
        }

        if ((uint64_t)cur->p_offset + cur->p_filesz > elf_len) {
            fprintf(stderr, "elf2kxe: load segment %" PRIu16 " is truncated\n", i);
            free(elf);
            return 1;
        }

        flags = kxe_flags_from_elf(cur->p_flags);
        if ((flags & (KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) ==
            (KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) {
            fprintf(stderr, "elf2kxe: refusing W+X segment %" PRIu16 "\n", i);
            free(elf);
            return 1;
        }

        memset(&plans[section_count], 0, sizeof(plans[section_count]));
        set_name(plans[section_count].sec.name, section_name_from_flags(flags, cur->p_filesz));
        plans[section_count].sec.type = cur->p_filesz == 0 ? KXE_SECTION_TYPE_NOBITS : KXE_SECTION_TYPE_LOAD;
        plans[section_count].sec.flags = flags;
        plans[section_count].sec.vaddr = cur->p_vaddr;
        plans[section_count].sec.vsize = cur->p_memsz;
        plans[section_count].sec.file_size = cur->p_filesz;
        plans[section_count].input_offset = cur->p_offset;

        if (plans[section_count].sec.type == KXE_SECTION_TYPE_LOAD) {
            output_cursor = align_up(output_cursor, 16u);
            plans[section_count].sec.file_offset = output_cursor;
            output_cursor += cur->p_filesz;
        }

        seg_end = cur->p_vaddr + cur->p_memsz;
        if (cur->p_vaddr < image_min) {
            image_min = cur->p_vaddr;
        }
        if (seg_end > image_max) {
            image_max = seg_end;
        }

        section_count++;
    }

    if (section_count == 0) {
        fprintf(stderr, "elf2kxe: no PT_LOAD segments found in %s\n", argv[1]);
        free(elf);
        return 1;
    }

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, KXE_MAGIC, 4);
    hdr.version_major = KXE_VERSION_MAJOR;
    hdr.version_minor = KXE_VERSION_MINOR;
    hdr.flags = KXE_FLAG_NONE;
    hdr.header_size = KXE_HEADER_SIZE;
    hdr.section_entry_size = KXE_SECTION_SIZE;
    hdr.section_count = section_count;
    hdr.abi_version = 1;
    hdr.entry_vaddr = eh->e_entry;
    hdr.section_table_offset = KXE_HEADER_SIZE;
    hdr.image_min_vaddr = image_min;
    hdr.image_max_vaddr = image_max;
    hdr.file_size = output_cursor;

    memset(header_page, 0, sizeof(header_page));
    memcpy(header_page, &hdr, sizeof(hdr));
    for (uint16_t i = 0; i < section_count; i++) {
        memcpy(header_page + hdr.section_table_offset + ((uint64_t)i * KXE_SECTION_SIZE),
               &plans[i].sec, sizeof(plans[i].sec));
    }

    hdr.header_crc32 = 0;
    hdr.header_crc32 = crc32_ieee(&hdr, sizeof(hdr));
    memcpy(header_page, &hdr, sizeof(hdr));

    out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "elf2kxe: failed to open %s for writing: %s\n", argv[2], strerror(errno));
        free(elf);
        return 1;
    }

    if (fwrite(header_page, 1, sizeof(header_page), out) != sizeof(header_page)) {
        fprintf(stderr, "elf2kxe: failed to write header page\n");
        fclose(out);
        free(elf);
        return 1;
    }

    for (uint16_t i = 0; i < section_count; i++) {
        uint64_t pos = (uint64_t)ftell(out);
        if (plans[i].sec.type != KXE_SECTION_TYPE_LOAD || plans[i].sec.file_size == 0) {
            continue;
        }

        if (pos > plans[i].sec.file_offset) {
            fprintf(stderr, "elf2kxe: internal output offset mismatch\n");
            fclose(out);
            free(elf);
            return 1;
        }

        if (write_padding(out, plans[i].sec.file_offset - pos) != 0) {
            fprintf(stderr, "elf2kxe: failed to pad output\n");
            fclose(out);
            free(elf);
            return 1;
        }

        if (fwrite(elf + plans[i].input_offset, 1, plans[i].sec.file_size, out) != plans[i].sec.file_size) {
            fprintf(stderr, "elf2kxe: failed to write section payload\n");
            fclose(out);
            free(elf);
            return 1;
        }
    }

    fclose(out);
    free(elf);
    return 0;
}
