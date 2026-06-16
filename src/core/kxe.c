#include "core/kxe.h"
#include <stddef.h>
#include "core/log.h"
#include "libc/crc32.h"
#include "libc/string.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "vfs/vfs.h"

static bool u64_add_overflow(uint64_t a, uint64_t b, uint64_t* out) {
    *out = a + b;
    return *out < a;
}

static bool u64_mul_overflow(uint64_t a, uint64_t b, uint64_t* out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return false;
    }

    if (a > (UINT64_MAX / b)) {
        return true;
    }

    *out = a * b;
    return false;
}

static bool addr_in_user_range(uint64_t addr) {
    return addr >= KXE_USER_MIN_VADDR && addr < KXE_USER_MAX_VADDR;
}

static bool range_contains(uint64_t start, uint64_t end_excl, uint64_t addr) {
    return start <= addr && addr < end_excl;
}

static bool range_overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

static const char* path_basename(const char* path) {
    const char* last = path;
    if (!path) {
        return "proc";
    }

    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }

    return *last ? last : "proc";
}

static bool vnode_read_exact(vnode_t* vn, uint64_t offset, void* buf, uint64_t len) {
    uint8_t* out = (uint8_t*)buf;
    uint64_t done = 0;

    if (!vn || !vn->ops || !vn->ops->read) {
        return false;
    }

    while (done < len) {
        int64_t n = vn->ops->read(vn, offset + done, out + done, len - done);
        if (n <= 0) {
            return false;
        }
        done += (uint64_t)n;
    }

    return true;
}

static bool section_validate(const kxe_header_t* hdr,
                             const kxe_section_t* sec,
                             uint64_t actual_file_size) {
    uint64_t sec_end = 0;

    if (sec->type != KXE_SECTION_TYPE_LOAD && sec->type != KXE_SECTION_TYPE_NOBITS) {
        return false;
    }

    if (sec->flags & ~(KXE_SECTION_FLAG_READ | KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) {
        return false;
    }

    if (sec->reserved != 0) {
        return false;
    }

    if (sec->vsize == 0) {
        return false;
    }

    if ((sec->flags & (KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) ==
        (KXE_SECTION_FLAG_WRITE | KXE_SECTION_FLAG_EXEC)) {
        return false;
    }

    if ((sec->vaddr & (PAGE_SIZE - 1)) != 0) {
        return false;
    }

    if (!addr_in_user_range(sec->vaddr)) {
        return false;
    }

    if (u64_add_overflow(sec->vaddr, sec->vsize, &sec_end)) {
        return false;
    }

    if (sec_end > KXE_USER_MAX_VADDR) {
        return false;
    }

    if (sec->vaddr < hdr->image_min_vaddr || sec_end > hdr->image_max_vaddr) {
        return false;
    }

    if (sec->type == KXE_SECTION_TYPE_LOAD) {
        uint64_t file_end = 0;

        if (sec->file_size > sec->vsize) {
            return false;
        }

        if (sec->file_size > 0 && sec->file_offset < KXE_HEADER_PAGE_SIZE) {
            return false;
        }

        if (u64_add_overflow(sec->file_offset, sec->file_size, &file_end)) {
            return false;
        }

        if (file_end > actual_file_size) {
            return false;
        }
    } else {
        if (sec->file_size != 0) {
            return false;
        }
    }

    return true;
}

bool kxe_validate(const uint8_t* header_page, uint64_t actual_file_size, kxe_image_t* out_image) {
    kxe_header_t hdr;
    kxe_header_t crc_hdr;
    uint32_t crc = 0;
    uint64_t table_bytes = 0;
    bool found_exec_entry = false;

    if (!header_page || !out_image) {
        return false;
    }

    if (actual_file_size < KXE_HEADER_PAGE_SIZE) {
        log_error("kxe", "File is smaller than the required 4 KiB KXE header page");
        return false;
    }

    memcpy(&hdr, header_page, sizeof(hdr));
    if (memcmp(hdr.magic, KXE_MAGIC, 4) != 0) {
        log_error("kxe", "Bad KXE magic");
        return false;
    }

    if (hdr.version_major != KXE_VERSION_MAJOR || hdr.version_minor != KXE_VERSION_MINOR) {
        log_errorf("kxe", "Unsupported KXE version %u.%u",
                   (unsigned)hdr.version_major, (unsigned)hdr.version_minor);
        return false;
    }

    if (hdr.flags != KXE_FLAG_NONE) {
        log_errorf("kxe", "Unsupported KXE header flags: %x", (unsigned)hdr.flags);
        return false;
    }

    if (hdr.header_size != KXE_HEADER_SIZE || hdr.section_entry_size != KXE_SECTION_SIZE) {
        log_error("kxe", "Unexpected KXE header or section entry size");
        return false;
    }

    if (hdr.section_count == 0 || hdr.section_count > KXE_MAX_SECTIONS) {
        log_error("kxe", "Invalid KXE section count");
        return false;
    }

    if (hdr.section_table_offset < hdr.header_size) {
        log_error("kxe", "Invalid KXE section table offset");
        return false;
    }

    if (u64_mul_overflow((uint64_t)hdr.section_count, hdr.section_entry_size, &table_bytes)) {
        log_error("kxe", "KXE section table overflow");
        return false;
    }

    if (u64_add_overflow(hdr.section_table_offset, table_bytes, &table_bytes)) {
        log_error("kxe", "KXE section table range overflow");
        return false;
    }

    if (table_bytes > KXE_HEADER_PAGE_SIZE) {
        log_error("kxe", "KXE section table does not fit in the first page");
        return false;
    }

    if (!addr_in_user_range(hdr.image_min_vaddr) ||
        hdr.image_max_vaddr > KXE_USER_MAX_VADDR ||
        hdr.image_min_vaddr >= hdr.image_max_vaddr) {
        log_error("kxe", "Invalid KXE image virtual address bounds");
        return false;
    }

    if (hdr.file_size != actual_file_size) {
        log_error("kxe", "KXE header file size does not match vnode size");
        return false;
    }

    crc_hdr = hdr;
    crc_hdr.header_crc32 = 0;
    crc = crc32_ieee(&crc_hdr, sizeof(crc_hdr));
    if (crc != hdr.header_crc32) {
        log_error("kxe", "KXE header CRC mismatch");
        return false;
    }

    memset(out_image, 0, sizeof(*out_image));
    out_image->header = hdr;

    for (uint16_t i = 0; i < hdr.section_count; i++) {
        uint64_t entry_off = hdr.section_table_offset + ((uint64_t)i * hdr.section_entry_size);
        uint64_t sec_end = 0;
        uint64_t sec_page_end = 0;
        uint64_t sec_page_start = 0;

        memcpy(&out_image->sections[i], header_page + entry_off, sizeof(kxe_section_t));
        if (!section_validate(&hdr, &out_image->sections[i], actual_file_size)) {
            log_errorf("kxe", "Section %u failed validation", (unsigned)i);
            return false;
        }

        if (u64_add_overflow(out_image->sections[i].vaddr, out_image->sections[i].vsize, &sec_end)) {
            log_error("kxe", "KXE section virtual range overflow");
            return false;
        }

        sec_page_start = PAGE_ALIGN_DOWN(out_image->sections[i].vaddr);
        sec_page_end = PAGE_ALIGN_UP(sec_end);

        for (uint16_t j = 0; j < i; j++) {
            uint64_t other_end = 0;
            uint64_t other_page_start = PAGE_ALIGN_DOWN(out_image->sections[j].vaddr);
            uint64_t other_page_end = 0;

            if (u64_add_overflow(out_image->sections[j].vaddr, out_image->sections[j].vsize, &other_end)) {
                log_error("kxe", "KXE section overlap check overflow");
                return false;
            }

            other_page_end = PAGE_ALIGN_UP(other_end);
            if (range_overlaps(sec_page_start, sec_page_end, other_page_start, other_page_end)) {
                log_errorf("kxe", "Sections %u and %u overlap in memory", (unsigned)j, (unsigned)i);
                return false;
            }
        }

        if ((out_image->sections[i].flags & KXE_SECTION_FLAG_EXEC) &&
            range_contains(out_image->sections[i].vaddr, sec_end, hdr.entry_vaddr)) {
            found_exec_entry = true;
        }
    }

    if (!found_exec_entry) {
        log_error("kxe", "KXE entry point does not land in an executable section");
        return false;
    }

    return true;
}

static bool kxe_load_section(process_t* proc, vnode_t* file, const kxe_section_t* sec) {
    uint64_t mapped_bytes = PAGE_ALIGN_UP(sec->vsize);
    uint64_t map_flags = PAGE_PRESENT | PAGE_USER;
    uint64_t phys_base = 0;
    size_t page_count = 0;

    if (!proc || !proc->page_table || !file || !sec) {
        return false;
    }

    if (mapped_bytes == 0 || (mapped_bytes / PAGE_SIZE) > SIZE_MAX) {
        return false;
    }

    page_count = (size_t)(mapped_bytes / PAGE_SIZE);
    phys_base = (uint64_t)(uintptr_t)pmm_alloc_pages(page_count);
    if (!phys_base) {
        return false;
    }

    memset(phys_to_virt(phys_base), 0, (size_t)mapped_bytes);

    if (sec->type == KXE_SECTION_TYPE_LOAD && sec->file_size > 0) {
        if (!vnode_read_exact(file, sec->file_offset, phys_to_virt(phys_base), sec->file_size)) {
            pmm_free_pages((void*)(uintptr_t)phys_base, page_count);
            return false;
        }
    }

    if (sec->flags & KXE_SECTION_FLAG_WRITE) {
        map_flags |= PAGE_WRITE;
    }

    for (size_t i = 0; i < page_count; i++) {
        uint64_t virt = sec->vaddr + ((uint64_t)i * PAGE_SIZE);
        uint64_t phys = phys_base + ((uint64_t)i * PAGE_SIZE);
        if (!vmm_map_page(proc->page_table, virt, phys, map_flags)) {
            for (size_t j = 0; j < i; j++) {
                uint64_t undo_virt = sec->vaddr + ((uint64_t)j * PAGE_SIZE);
                vmm_unmap_page(proc->page_table, undo_virt);
            }
            pmm_free_pages((void*)(uintptr_t)phys_base, page_count);
            return false;
        }
    }

    return true;
}

process_t* kxe_load(const char* path) {
    vnode_t* file = NULL;
    uint64_t stack_base = KXE_USER_STACK_TOP - ((uint64_t)KXE_USER_STACK_PAGES * PAGE_SIZE);
    uint8_t* header_page = NULL;
    kxe_image_t* image = NULL;
    process_t* proc = NULL;

    if (!path || path[0] != '/') {
        log_error("kxe", "kxe_load requires an absolute path");
        return NULL;
    }

    if (!vfs_resolve(path, &file) || !file) {
        log_errorf("kxe", "Failed to resolve %s", path);
        return NULL;
    }

    if (file->type != VNODE_FILE || !file->ops || !file->ops->read) {
        log_errorf("kxe", "%s is not a readable file", path);
        vfs_vnode_put(file);
        return NULL;
    }

    header_page = (uint8_t*)kmalloc(KXE_HEADER_PAGE_SIZE);
    image = (kxe_image_t*)kmalloc(sizeof(*image));
    if (!header_page || !image) {
        log_error("kxe", "Out of memory allocating loader scratch buffers");
        if (header_page) {
            kfree(header_page);
        }
        if (image) {
            kfree(image);
        }
        vfs_vnode_put(file);
        return NULL;
    }

    if (!vnode_read_exact(file, 0, header_page, KXE_HEADER_PAGE_SIZE)) {
        log_errorf("kxe", "Failed to read KXE header page from %s", path);
        kfree(image);
        kfree(header_page);
        vfs_vnode_put(file);
        return NULL;
    }

    if (!kxe_validate(header_page, file->size, image)) {
        kfree(image);
        kfree(header_page);
        vfs_vnode_put(file);
        return NULL;
    }

    if (image->header.image_max_vaddr > stack_base) {
        log_error("kxe", "KXE image overlaps the fixed user stack region");
        kfree(image);
        kfree(header_page);
        vfs_vnode_put(file);
        return NULL;
    }

    proc = process_create(path_basename(path));
    if (!proc) {
        log_error("kxe", "Failed to create process for KXE image");
        kfree(image);
        kfree(header_page);
        vfs_vnode_put(file);
        return NULL;
    }

    for (uint16_t i = 0; i < image->header.section_count; i++) {
        if (!kxe_load_section(proc, file, &image->sections[i])) {
            log_errorf("kxe", "Failed to load section %u from %s", (unsigned)i, path);
            kfree(image);
            kfree(header_page);
            vfs_vnode_put(file);
            process_destroy(proc);
            return NULL;
        }
    }

    if (!process_map_user_stack(proc, KXE_USER_STACK_TOP, KXE_USER_STACK_PAGES)) {
        log_error("kxe", "Failed to map user stack for KXE image");
        kfree(image);
        kfree(header_page);
        vfs_vnode_put(file);
        process_destroy(proc);
        return NULL;
    }

    proc->context.rip = image->header.entry_vaddr;
    proc->context.rsp = KXE_USER_STACK_TOP;
    proc->context.rflags = 0x202;
    proc->brk_base = PAGE_ALIGN_UP(image->header.image_max_vaddr);
    proc->brk_current = proc->brk_base;
    proc->state = PROC_READY;

    kfree(image);
    kfree(header_page);
    vfs_vnode_put(file);
    return proc;
}
