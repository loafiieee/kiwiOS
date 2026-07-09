#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/apic.h"
#include "arch/x86/hpet.h"
#include "arch/x86/io.h"
#include "core/boot.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/kxe.h"
#include "core/log.h"
#include "core/scheduler.h"
#include "core/usertest.h"
#include "drivers/acpi/acpi.h"
#include "drivers/block/block.h"
#include "drivers/usb/usb_storage.h"
#include "libc/string.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/hhdm.h"
#include "fs/bcache.h"
#include "vfs/vfs.h"
#include "fs/kifs/kifs.h"
#include "fs/kifs/kifs_disk.h"

static void print_byte_hex(struct limine_framebuffer *fb, uint8_t b) {
    static const char* hex = "0123456789ABCDEF";
    putc_fb(fb, hex[(b >> 4) & 0xF]);
    putc_fb(fb, hex[b & 0xF]);
}

static bool parse_u64(const char* s, uint64_t* out) {
    if (!s) return false;
    while (*s == ' ') s++;
    if (*s == '\0') return false;

    uint64_t v = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') {
        any = true;
        v = v * 10 + (uint64_t)(*s - '0');
        s++;
    }
    if (!any) return false;
    *out = v;
    return true;
}

static bool parse_u32(const char* s, uint32_t* out) {
    uint64_t v = 0;
    if (!parse_u64(s, &v)) return false;
    if (v > 0xFFFFFFFFULL) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u8(const char* s, uint8_t* out) {
    uint64_t v = 0;
    if (!parse_u64(s, &v)) return false;
    if (v > 0xFFULL) return false;
    *out = (uint8_t)v;
    return true;
}

static const char* skip_token(const char* s) {
    if (!s) return s;
    while (*s == ' ') s++;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return s;
}

static bool parse_u32_strict(const char* s, uint32_t* out) {
    if (!s || !out) {
        return false;
    }

    while (*s == ' ') s++;
    if (*s == '\0') {
        return false;
    }

    if (!parse_u32(s, out)) {
        return false;
    }

    while (*s >= '0' && *s <= '9') {
        s++;
    }

    return *s == '\0';
}

static const char* trim_spaces(const char* s);
static bool copy_next_token(const char** s, char* out, uint32_t out_cap);
static bool normalize_shell_path(const char* in, char* out, uint32_t out_cap);
static bool resolve_program_path(const char* in, char* out, uint32_t out_cap);

static char g_shell_cwd[256] = "/";

// ================= Command functions =================
static void cmd_help(struct limine_framebuffer *fb) {
    print(fb, "Built-ins:\n");
    print(fb, "  help clear echo about crash meminfo memtest vmtest heaptest fbinfo scale\n");
    print(fb, "  partlist rescan usb acpi apic hpet reboot poweroff disktest rawread rawwrite rawflush diskreadp diskwritep diskflushp\n");
    print(fb, "  bcachestat bcacheflush bcacheflushp mount kifs kifsbmdump mkfs.kifs\n");
    print(fb, "  pwd cd ls stat cat touch mkdir rm cp mv exec usertest\n");
    print(fb, "Recovery:\n");
    print(fb, "  mkfs.kifs /dev/disk0 --kiwios-root    format a whole disk as KiwiOS root\n");
    print(fb, "  mkfs.kifs /dev/disk0p1 --kiwios-root  format a partition as KiwiOS root\n");
    print(fb, "  mount /dev/disk0 /                    mount or remount root\n");
    print(fb, "  exec /bin/sh                          start userspace shell if installed\n");
    print(fb, "If /bin exists, unknown commands fall through to /bin/<command>.\n");
}










static void cmd_clear(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    console_clear();
}

static void cmd_echo(struct limine_framebuffer *fb, const char *args) {
    if (args && *args) {
        print(fb, args);
        print(fb, "\n");
    } else {
        print(fb, "\n");
    }
}

static void cmd_about(struct limine_framebuffer *fb) {
    print(fb, "KiwiOS v0.1\n");
    print(fb, "A simple operating system\n");
}

static void cmd_crash(struct limine_framebuffer *fb, const char *args) {
    int exception_num = 0; // default to divide by zero

    // Parse exception number from args
    if (args && *args) {
        exception_num = 0; // Reset to 0 when parsing
        // Simple string to int conversion
        while (*args >= '0' && *args <= '9') {
            exception_num = exception_num * 10 + (*args - '0');
            args++;
        }
    }

    print(fb, "Triggering exception ");
    print_hex(fb, exception_num);
    print(fb, "...\n");

    // Trigger the appropriate exception
    switch (exception_num) {
        case 0: { // Division by zero
            volatile int x = 1;
            volatile int y = 0;
            volatile int z = x / y;
            (void)z;
            break;
        }
        case 1: // Debug - use int instruction
            asm volatile ("int $1");
            break;
        case 2: // Non Maskable Interrupt
            asm volatile ("int $2");
            break;
        case 3: // Breakpoint
            asm volatile ("int3");
            break;
        case 4: // Overflow
            asm volatile ("int $4");
            break;
        case 5: { // Bound range exceeded
            asm volatile ("int $5");
            break;
        }
        case 6: // Invalid opcode
            asm volatile ("ud2");
            break;
        case 7: { // Device not available (FPU)
            asm volatile (
                "clts\n"
                "fninit\n"
                "mov $0, %%rax\n"
                "mov %%rax, %%cr0\n"
                "fld1\n"
                ::: "rax"
            );
            asm volatile ("int $7");
            break;
        }
        case 8:
            asm volatile ("int $8");
            break;
        case 10:
            asm volatile ("int $10");
            break;
        case 11:
            asm volatile ("int $11");
            break;
        case 12:
            asm volatile ("int $12");
            break;
        case 13: {
            asm volatile ("mov $0xFFFF, %%ax; mov %%ax, %%ds" ::: "rax");
            break;
        }
        case 14: {
            volatile uint64_t *ptr = (uint64_t *)0xFFFFFFFF80000000ULL;
            volatile uint64_t val = *ptr;
            (void)val;
            break;
        }
        case 16:
            asm volatile ("int $16");
            break;
        case 17:
            asm volatile ("int $17");
            break;
        case 18:
            asm volatile ("int $18");
            break;
        case 19:
            asm volatile ("int $19");
            break;
        case 20:
            asm volatile ("int $20");
            break;
        case 21:
            asm volatile ("int $21");
            break;
        default:
            print(fb, "Exception number not supported or reserved.\n");
            print(fb, "Supported: 0-8, 10-14, 16-21\n");
            return;
    }
}

static void cmd_meminfo(struct limine_framebuffer *fb) {
    size_t total, used, free;
    pmm_get_stats(&total, &used, &free);

    print(fb, "Memory Information:\n");
    print(fb, "  Total pages: ");
    print_hex(fb, total);
    print(fb, " (");
    print_hex(fb, total * 4); // KB
    print(fb, " KB)\n");

    print(fb, "  Used pages:  ");
    print_hex(fb, used);
    print(fb, " (");
    print_hex(fb, used * 4);
    print(fb, " KB)\n");

    print(fb, "  Free pages:  ");
    print_hex(fb, free);
    print(fb, " (");
    print_hex(fb, free * 4);
    print(fb, " KB)\n");
}

static void cmd_memtest(struct limine_framebuffer *fb) {
    print(fb, "Testing memory allocation...\n");

    // Allocate a single page
    void* page1 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page1);
    print(fb, "\n");

    // Allocate another page
    void* page2 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page2);
    print(fb, "\n");

    // Allocate 10 contiguous pages
    void* pages = pmm_alloc_pages(10);
    if (pages) {
        print(fb, "Allocated 10 pages at: ");
        print_hex(fb, (uint64_t)pages);
        print(fb, "\n");
    } else {
        print(fb, "Failed to allocate 10 pages!\n");
    }

    // Free them
    print(fb, "Freeing allocations...\n");
    pmm_free(page1);
    pmm_free(page2);
    if (pages) pmm_free_pages(pages, 10);

    print(fb, "Memory test complete!\n");
}

static void cmd_vmtest(struct limine_framebuffer *fb) {
    print(fb, "Testing Virtual Memory Manager...\n");

    // Create a new page table
    page_table_t* test_pt = vmm_create_page_table();
    if (!test_pt) {
        print(fb, "Failed to create page table!\n");
        return;
    }
    print(fb, "Created page table at: ");
    print_hex(fb, (uint64_t)test_pt);
    print(fb, "\n");

    // Allocate a physical page
    uint64_t phys_page = (uint64_t)pmm_alloc();
    if (!phys_page) {
        print(fb, "Failed to allocate physical page!\n");
        return;
    }
    print(fb, "Allocated physical page: ");
    print_hex(fb, phys_page);
    print(fb, "\n");

    // Map it to a virtual address
    uint64_t virt_addr = 0x400000;
    bool mapped = vmm_map_page(test_pt, virt_addr, phys_page, PAGE_WRITE | PAGE_USER);
    if (!mapped) {
        print(fb, "Failed to map page!\n");
        pmm_free((void*)phys_page);
        return;
    }
    print(fb, "Mapped virtual ");
    print_hex(fb, virt_addr);
    print(fb, " -> physical ");
    print_hex(fb, phys_page);
    print(fb, "\n");

    // Verify the mapping
    uint64_t phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == phys_page) {
        print(fb, "Mapping verified successfully!\n");
    } else {
        print(fb, "Mapping verification FAILED!\n");
        print(fb, "Expected: ");
        print_hex(fb, phys_page);
        print(fb, "\nGot: ");
        print_hex(fb, phys_result);
        print(fb, "\n");
    }

    // Test unmapping
    vmm_unmap_page(test_pt, virt_addr);
    phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == 0) {
        print(fb, "Unmapping successful!\n");
    } else {
        print(fb, "Unmapping FAILED!\n");
    }

    // Clean up
    pmm_free((void*)phys_page);

    print(fb, "VMM test complete!\n");
}

static void cmd_heaptest(struct limine_framebuffer *fb) {
    print(fb, "Testing heap allocator...\n");

    // Test 1: Simple allocation
    char* str1 = (char*)kmalloc(32);
    if (str1) {
        print(fb, "Allocated 32 bytes at: ");
        print_hex(fb, (uint64_t)str1);
        print(fb, "\n");
    }

    // Test 2: Multiple allocations
    int* numbers = (int*)kmalloc(10 * sizeof(int));
    if (numbers) {
        print(fb, "Allocated array at: ");
        print_hex(fb, (uint64_t)numbers);
        print(fb, "\n");
    }

    // Test 3: Calloc (zeroed memory)
    uint64_t* zeroed = (uint64_t*)kcalloc(5, sizeof(uint64_t));
    if (zeroed) {
        print(fb, "Allocated zeroed memory at: ");
        print_hex(fb, (uint64_t)zeroed);
        print(fb, "\n");
    }

    // Show stats
    size_t allocated, free_mem, allocs;
    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "Heap stats:\n");
    print(fb, "  Allocated: ");
    print_hex(fb, allocated);
    print(fb, " bytes\n");
    print(fb, "  Free: ");
    print_hex(fb, free_mem);
    print(fb, " bytes\n");
    print(fb, "  Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");

    // Free everything
    kfree(str1);
    kfree(numbers);
    kfree(zeroed);

    print(fb, "Freed all allocations\n");

    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "After free - Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");
}

static void cmd_fbinfo(struct limine_framebuffer *fb_unused) {
    (void)fb_unused;

    struct limine_framebuffer_response *response = boot_framebuffer_response();
    if (!response || response->framebuffer_count == 0) {
        print(NULL, "No framebuffers from Limine.\n");
        return;
    }

    uint64_t count = response->framebuffer_count;
    print(NULL, "Framebuffers: ");
    print_u64(NULL, count);
    print(NULL, "\n");

    for (uint64_t i = 0; i < count; i++) {
        struct limine_framebuffer *fb = response->framebuffers[i];
        if (!fb) continue;

        print(NULL, "FB#"); print_u64(NULL, i); print(NULL, ": ");
        print_u64(NULL, fb->width);  print(NULL, "x");
        print_u64(NULL, fb->height); print(NULL, "@");
        print_u64(NULL, fb->bpp);    print(NULL, "  pitch=");
        print_u64(NULL, fb->pitch);
        print(NULL, " bytes\n");

        print(NULL, "  mem_model=");
        print_u64(NULL, fb->memory_model);
        print(NULL, "  R(");
        print_u64(NULL, fb->red_mask_size);   print(NULL, ":");
        print_u64(NULL, fb->red_mask_shift);  print(NULL, ")  G(");
        print_u64(NULL, fb->green_mask_size); print(NULL, ":");
        print_u64(NULL, fb->green_mask_shift);print(NULL, ")  B(");
        print_u64(NULL, fb->blue_mask_size);  print(NULL, ":");
        print_u64(NULL, fb->blue_mask_shift); print(NULL, ")\n");

        print(NULL, "  edid=");
        if (fb->edid && fb->edid_size) {
            print_u64(NULL, fb->edid_size); print(NULL, " bytes\n");
        } else {
            print(NULL, "none\n");
        }

        if (fb->mode_count && fb->modes) {
            uint64_t mcount = fb->mode_count;
            print(NULL, "  modes=");
            print_u64(NULL, mcount);
            print(NULL, " (showing up to 10)\n");

            uint64_t show = mcount > 10 ? 10 : mcount;
            for (uint64_t j = 0; j < show; j++) {
                struct limine_video_mode *m = fb->modes[j];
                if (!m) continue;

                print(NULL, "    [");
                print_u64(NULL, j);
                print(NULL, "] ");
                print_u64(NULL, m->width);  print(NULL, "x");
                print_u64(NULL, m->height); print(NULL, "@");
                print_u64(NULL, m->bpp);
                print(NULL, "  pitch=");
                print_u64(NULL, m->pitch);
                print(NULL, "  mem_model=");
                print_u64(NULL, m->memory_model);
                print(NULL, "\n");
            }
        } else {
            print(NULL, "  modes=none\n");
        }

        print(NULL, "\n");
    }
}

static void cmd_scale(struct limine_framebuffer *fb, const char *args) {
    (void)fb;
    uint32_t s = 0;
    if (args) {
        while (*args == ' ') args++;
        while (*args >= '0' && *args <= '9') {
            s = s * 10 + (uint32_t)(*args - '0');
            args++;
        }
    }
    if (s == 0) s = 1;
    if (s > 16) s = 9;

    console_set_scale(s);

    print(NULL, "scale set to ");
    char buf[12]; int i = 0; uint32_t t = s; do { buf[i++] = '0' + (t % 10); t/=10; } while (t);
    while (i--) putc_fb(NULL, buf[i]);
    print(NULL, "x\n");
}

// -------- Device helpers --------

static block_device_t* must_get_bootdev(struct limine_framebuffer *fb) {
    block_device_t* dev = block_boot_device();
    if (!dev) {
        print(fb, "No boot block device (block_init failed?)\n");
        return NULL;
    }
    if (!dev->read || !dev->write) {
        print(fb, "Boot block device missing read/write\n");
        return NULL;
    }
    if (dev->sector_size == 0) {
        print(fb, "Boot block device has invalid sector size\n");
        return NULL;
    }
    return dev;
}

static block_device_t* get_part(struct limine_framebuffer *fb, uint32_t idx) {
    (void)fb;
    return block_partition_device(idx);
}

static block_device_t* resolve_block_device_spec(const char* spec, bool allow_partition_index) {
    const char* name = spec;
    uint32_t part_index = 0;

    if (!spec || !*spec) {
        return NULL;
    }

    if (allow_partition_index && parse_u32_strict(spec, &part_index)) {
        return block_partition_device(part_index);
    }

    if (strncmp(name, "/dev/", 5u) == 0) {
        name += 5u;
    }

    return block_device_by_name(name);
}

// -------- Core disk ops (shared) --------

static bool do_diskread(struct limine_framebuffer* fb, block_device_t* dev, uint64_t lba, uint32_t count) {
    uint32_t bytes = count * dev->sector_size;

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void* phys = pmm_alloc_pages(pages);
    if (!phys) {
        print(fb, "diskread: pmm_alloc_pages failed\n");
        return false;
    }

    void* buf = hhdm_phys_to_virt((uint64_t)(uintptr_t)phys);
    memset(buf, 0, pages * PAGE_SIZE);

    bool ok = dev->read(dev, lba, count, buf);
    if (!ok) {
        print(fb, "diskread: read failed (check logs)\n");
        pmm_free_pages(phys, pages);
        return false;
    }

    print(fb, "Read OK. First 256 bytes:\n");
    uint8_t* b = (uint8_t*)buf;
    uint32_t show = bytes < 256u ? bytes : 256u;

    for (uint32_t i = 0; i < show; i++) {
        if ((i % 16) == 0) {
            print(fb, "\n");
            print_hex(fb, i);
            print(fb, ": ");
        }
        print_byte_hex(fb, b[i]);
        putc_fb(fb, ' ');
    }
    print(fb, "\n\n");

    pmm_free_pages(phys, pages);
    return true;
}

static bool do_diskwrite(struct limine_framebuffer* fb, block_device_t* dev, uint64_t lba, uint32_t count, uint8_t pattern) {
    const uint32_t MAX_BYTES = 512u * 1024u;
    uint32_t bytes = count * dev->sector_size;
    if (bytes > MAX_BYTES) {
        print(fb, "diskwrite: request too large (cap = 512KiB). Reduce count.\n");
        return false;
    }

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void* phys_w = pmm_alloc_pages(pages);
    void* phys_r = pmm_alloc_pages(pages);
    if (!phys_w || !phys_r) {
        print(fb, "diskwrite: pmm_alloc_pages failed\n");
        if (phys_w) pmm_free_pages(phys_w, pages);
        if (phys_r) pmm_free_pages(phys_r, pages);
        return false;
    }

    void* buf_w = phys_to_virt((uint64_t)(uintptr_t)phys_w);
    void* buf_r = phys_to_virt((uint64_t)(uintptr_t)phys_r);

    memset(buf_w, pattern, pages * PAGE_SIZE);
    memset(buf_r, 0,       pages * PAGE_SIZE);

    print(fb, "Writing...\n");
    if (!dev->write(dev, lba, count, buf_w)) {
        print(fb, "diskwrite: write failed (check logs)\n");
        pmm_free_pages(phys_w, pages);
        pmm_free_pages(phys_r, pages);
        return false;
    }

    print(fb, "Reading back...\n");
    if (!dev->read(dev, lba, count, buf_r)) {
        print(fb, "diskwrite: read-back failed (check logs)\n");
        pmm_free_pages(phys_w, pages);
        pmm_free_pages(phys_r, pages);
        return false;
    }

    uint8_t* w = (uint8_t*)buf_w;
    uint8_t* r = (uint8_t*)buf_r;

    for (uint32_t i = 0; i < bytes; i++) {
        if (w[i] != r[i]) {
            print(fb, "VERIFY FAILED at byte ");
            print_hex(fb, i);
            print(fb, ": wrote ");
            print_byte_hex(fb, w[i]);
            print(fb, " read ");
            print_byte_hex(fb, r[i]);
            print(fb, "\n");
            pmm_free_pages(phys_w, pages);
            pmm_free_pages(phys_r, pages);
            return false;
        }
    }

    print(fb, "VERIFY OK\n");

    pmm_free_pages(phys_w, pages);
    pmm_free_pages(phys_r, pages);
    return true;
}

// -------- Commands --------

static void cmd_rawread(struct limine_framebuffer *fb, const char *args) {
    print(fb, "WARNING, reading from RAW DISK, not a partition!\n");
    print(fb, "Press ENTER to continue...\n");
    int c = keyboard_getchar();
    if (c != '\n') {
        print(fb, "\nRead aborted by user.\n");
        return;
    }
    block_device_t* dev = must_get_bootdev(fb);
    if (!dev) return;

    uint64_t lba = 0;
    uint32_t count = 1;

    if (!parse_u64(args, &lba)) {
        print(fb, "Usage: rawread <lba> [count]\n");
        return;
    }

    const char* a2 = skip_token(args);
    if (a2 && *a2) {
        (void)parse_u32(a2, &count);
        if (count == 0) count = 1;
    }

    (void)do_diskread(fb, dev, lba, count);
}

static void cmd_rawwrite(struct limine_framebuffer *fb, const char *args) {
    print(fb, "WARNING, writing to RAW DISK, not a partition!\n");
    print(fb, "Press ENTER to continue...\n");
    int c = keyboard_getchar();
    if (c != '\n') {
        print(fb, "\nWrite aborted by user.\n");
        return;
    }
    block_device_t* dev = must_get_bootdev(fb);
    if (!dev) return;

    uint64_t lba = 0;
    uint32_t count = 1;
    uint8_t pattern = 0xAA;

    if (!parse_u64(args, &lba)) {
        print(fb, "Usage: rawwrite <lba> [count] <byte>\n");
        return;
    }

    const char* a2 = skip_token(args);
    const char* a3 = skip_token(a2);

    if (a2 && *a2) {
        (void)parse_u32(a2, &count);
        if (count == 0) count = 1;
    }

    if (a3 && *a3) {
        (void)parse_u8(a3, &pattern);
    } else {
        // convenience: diskwrite <lba> <byte>
        uint8_t maybe_pat = 0;
        if (a2 && *a2 && parse_u8(a2, &maybe_pat)) {
            pattern = maybe_pat;
            count = 1;
        }
    }

    (void)do_diskwrite(fb, dev, lba, count, pattern);
}

static void cmd_rawflush(struct limine_framebuffer *fb) {
    print(fb, "WARNING, writing to RAW DISK, not a partition!\n");
    print(fb, "Press ENTER to continue...\n");
    int c = keyboard_getchar();
    if (c != '\n') {
        print(fb, "\nFlush aborted by user.\n");
        return;
    }
    block_device_t* dev = must_get_bootdev(fb);
    if (!dev) return;

    if (!dev->flush) {
        print(fb, "rawflush: device does not support flush\n");
        return;
    }

    print(fb, "Flushing drive cache...\n");
    if (!dev->flush(dev)) {
        print(fb, "rawflush: flush failed (check logs)\n");
        return;
    }
    print(fb, "rawflush: OK\n");
}

static void cmd_partlist(struct limine_framebuffer* fb) {
    uint32_t disk_n = block_disk_count();
    uint32_t n = block_partition_count();

    print(fb, "Disks found: ");
    print_hex(fb, disk_n);
    print(fb, "\n");

    for (uint32_t i = 0; i < disk_n; i++) {
        block_device_t* d = block_disk_device(i);
        part_table_type_t t = block_disk_partition_table_type(i);
        if (!d) continue;

        print(fb, "  [");
        print_hex(fb, i);
        print(fb, "] ");
        print(fb, d->name ? d->name : "(noname)");
        print(fb, "  sectors=");
        print_hex(fb, d->total_sectors);
        print(fb, "  table=");
        switch (t) {
            case PART_TABLE_GPT: print(fb, "GPT"); break;
            case PART_TABLE_MBR: print(fb, "MBR"); break;
            default:             print(fb, "none"); break;
        }
        print(fb, "\n");
    }

    print(fb, "Partitions found: ");
    print_hex(fb, n);
    print(fb, "\n");

    for (uint32_t i = 0; i < n; i++) {
        block_device_t* p = block_partition_device(i);
        if (!p) continue;

        // We can also show start LBA if you want later, but we keep this minimal.
        print(fb, "  [");
        print_hex(fb, i);
        print(fb, "] ");
        print(fb, p->name ? p->name : "(noname)");
        print(fb, "  sectors=");
        print_hex(fb, p->total_sectors);
        print(fb, "\n");
    }
}

static void cmd_rescan(struct limine_framebuffer* fb) {
    uint32_t added = block_rescan();

    print(fb, "rescan: found ");
    print_u32(fb, added);
    print(fb, " new disk(s)\n");
}

static void cmd_usb(struct limine_framebuffer* fb) {
    uint32_t controller_count = usb_controller_count();
    uint32_t storage_count = 0;

    (void)block_poll_hotplug();
    storage_count = usb_storage_disk_count();

    print(fb, "USB controllers: ");
    print_u32(fb, controller_count);
    print(fb, "\n");

    for (uint32_t i = 0; i < controller_count; i++) {
        usb_controller_info_t info;
        if (!usb_controller_info(i, &info)) {
            continue;
        }

        print(fb, "  [");
        print_u32(fb, i);
        print(fb, "] ");
        print(fb, usb_controller_type_name(info.type));
        print(fb, " pci=");
        print_hex(fb, info.bus);
        print(fb, ":");
        print_hex(fb, info.dev);
        print(fb, ".");
        print_u32(fb, info.func);
        print(fb, info.mmio ? " mmio=" : " io=");
        print_hex(fb, info.base);
        print(fb, info.supported ? " supported\n" : " unsupported\n");
    }

    print(fb, "USB storage devices: ");
    print_u32(fb, storage_count);
    print(fb, "\n");
    for (uint32_t i = 0; i < storage_count; i++) {
        print(fb, "  [");
        print_u32(fb, i);
        print(fb, "] sectors=");
        print_u64(fb, usb_storage_total_sectors(i));
        print(fb, "\n");
    }

    if (controller_count == 0u) {
        print(fb, "No PCI USB controller was detected.\n");
    } else if (storage_count == 0u) {
        print(fb, "No USB storage block device is available. If the controller is EHCI/xHCI, that driver is still missing.\n");
    }
}

static void cmd_acpi(struct limine_framebuffer* fb) {
    acpi_dump(fb);
}

static void cmd_apic(struct limine_framebuffer* fb) {
    apic_dump(fb);
}

static void cmd_hpet(struct limine_framebuffer* fb) {
    hpet_dump(fb);
}

static void legacy_i8042_reboot(void) {
    asm volatile("cli");

    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(0x64) & 0x02u) == 0u) {
            break;
        }
        asm volatile("pause");
    }

    outb(0x64, 0xFE);
    for (;;) {
        asm volatile("hlt");
    }
}

static void cmd_reboot(struct limine_framebuffer* fb) {
    print(fb, "reboot: trying ACPI reset\n");
    if (!acpi_reboot()) {
        print(fb, "reboot: ACPI reset unavailable or failed\n");
    } else {
        print(fb, "reboot: ACPI reset returned; trying fallback\n");
    }

    legacy_i8042_reboot();
}

static void cmd_poweroff(struct limine_framebuffer* fb) {
    print(fb, "poweroff: trying ACPI S5\n");
    if (!acpi_poweroff()) {
        print(fb, "poweroff: ACPI S5 unavailable or failed\n");
        return;
    }

    print(fb, "poweroff: ACPI S5 returned; hardware did not power off\n");
}

static void cmd_diskreadp(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    uint64_t lba = 0;
    uint32_t count = 1;

    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: diskreadp <part> <lba> [count]\n");
        return;
    }
    const char* a2 = skip_token(args);
    if (!parse_u64(a2, &lba)) {
        print(fb, "Usage: diskreadp <part> <lba> [count]\n");
        return;
    }
    const char* a3 = skip_token(a2);
    if (a3 && *a3) {
        (void)parse_u32(a3, &count);
        if (count == 0) count = 1;
    }

    block_device_t* dev = get_part(fb, idx);
    if (!dev) {
        print(fb, "diskreadp: invalid partition index\n");
        return;
    }
    (void)do_diskread(fb, dev, lba, count);
}

static void cmd_diskwritep(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    uint64_t lba = 0;
    uint32_t count = 1;
    uint8_t pattern = 0xAA;

    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: diskwritep <part> <lba> [count] <byte>\n");
        return;
    }
    const char* a2 = skip_token(args);
    if (!parse_u64(a2, &lba)) {
        print(fb, "Usage: diskwritep <part> <lba> [count] <byte>\n");
        return;
    }
    const char* a3 = skip_token(a2);
    const char* a4 = skip_token(a3);

    if (a3 && *a3) {
        (void)parse_u32(a3, &count);
        if (count == 0) count = 1;
    }
    if (a4 && *a4) {
        (void)parse_u8(a4, &pattern);
    } else {
        // allow: diskwritep <part> <lba> <byte>  (count=1)
        uint8_t maybe_pat = 0;
        if (a3 && *a3 && parse_u8(a3, &maybe_pat)) {
            pattern = maybe_pat;
            count = 1;
        } else {
            print(fb, "Usage: diskwritep <part> <lba> [count] <byte>\n");
            return;
        }
    }

    block_device_t* dev = get_part(fb, idx);
    if (!dev) {
        print(fb, "diskwritep: invalid partition index\n");
        return;
    }
    (void)do_diskwrite(fb, dev, lba, count, pattern);
}

static void cmd_diskflushp(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: diskflushp <part>\n");
        return;
    }
    block_device_t* dev = get_part(fb, idx);
    if (!dev) {
        print(fb, "diskflushp: invalid partition index\n");
        return;
    }
    if (!dev->flush) {
        print(fb, "diskflushp: device does not support flush\n");
        return;
    }
    print(fb, "Flushing...\n");
    if (!dev->flush(dev)) {
        print(fb, "diskflushp: flush failed\n");
        return;
    }
    print(fb, "diskflushp: OK\n");
}

static void cmd_disktest(struct limine_framebuffer *fb) {
    print(fb, "disktest: writing pattern 0x5A to LBA 2048, 1 sector\n");
    cmd_rawwrite(fb, "2048 1 90");
}

// -------- block cache --------

static void cmd_bcachestat(struct limine_framebuffer* fb) {
    bcache_stats_t s = bcache_stats();

    print(fb, "bcache:\n");
    print(fb, "  bufs:   used=");
    print_hex(fb, s.used_bufs);
    print(fb, " total=");
    print_hex(fb, s.total_bufs);
    print(fb, " dirty=");
    print_hex(fb, s.dirty_bufs);
    print(fb, "\n");

    print(fb, "  hits=");
    print_hex(fb, s.hits);
    print(fb, " misses=");
    print_hex(fb, s.misses);
    print(fb, " evictions=");
    print_hex(fb, s.evictions);
    print(fb, "\n");

    print(fb, "  writebacks=");
    print_hex(fb, s.writebacks);
    print(fb, " sync_calls=");
    print_hex(fb, s.sync_calls);
    print(fb, "\n");
}

static void cmd_bcacheflush(struct limine_framebuffer* fb) {
    print(fb, "bcacheflush: syncing all dirty buffers...\n");
    if (!bcache_sync_all()) {
        print(fb, "bcacheflush: FAILED (see logs)\n");
        return;
    }
    print(fb, "bcacheflush: OK\n");
}

static void cmd_bcacheflushp(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: bcacheflushp <part>\n");
        return;
    }

    block_device_t* dev = block_partition_device(idx);
    if (!dev) {
        print(fb, "bcacheflushp: invalid partition index\n");
        return;
    }

    print(fb, "bcacheflushp: syncing partition buffers...\n");
    if (!bcache_sync_dev(dev)) {
        print(fb, "bcacheflushp: FAILED (see logs)\n");
        return;
    }
    print(fb, "bcacheflushp: OK\n");
}


// -------- Filesystem (KiFS via VFS) --------

static const char* trim_spaces(const char* s) {
    if (!s) return s;
    while (*s == ' ') s++;
    return s;
}

static bool copy_next_token(const char** s, char* out, uint32_t out_cap) {
    const char* cur = NULL;
    uint32_t len = 0;
    char quote = '\0';

    if (!s || !out || out_cap == 0) return false;
    cur = trim_spaces(*s);
    if (!cur || *cur == '\0') return false;

    while (*cur) {
        if (quote) {
            if (*cur == quote) {
                quote = '\0';
                cur++;
                continue;
            }
            if (quote == '"' && *cur == '\\' && cur[1] != '\0') {
                cur++;
            }
        } else {
            if (*cur == ' ') {
                break;
            }
            if (*cur == '"' || *cur == '\'') {
                quote = *cur++;
                continue;
            }
            if (*cur == '\\' && cur[1] != '\0') {
                cur++;
            }
        }

        if (len + 1u >= out_cap) return false;
        out[len++] = *cur++;
    }

    out[len] = '\0';
    if (*cur != '\0') {
        cur++;
    }
    *s = trim_spaces(cur);
    return true;
}

static void shell_path_pop_component(char* path) {
    uint32_t len = 0;

    if (!path) return;
    len = (uint32_t)strlen(path);
    if (len <= 1u) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    while (len > 1u && path[len - 1u] == '/') {
        len--;
    }

    while (len > 1u && path[len - 1u] != '/') {
        len--;
    }

    if (len <= 1u) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    path[len - 1u] = '\0';
}

static bool shell_path_append_component(char* path, uint32_t out_cap, const char* comp, uint32_t comp_len) {
    uint32_t cur_len = 0;

    if (!path || !comp || comp_len == 0u) return true;

    if (comp_len == 1u && comp[0] == '.') {
        return true;
    }

    if (comp_len == 2u && comp[0] == '.' && comp[1] == '.') {
        shell_path_pop_component(path);
        return true;
    }

    cur_len = (uint32_t)strlen(path);
    if (cur_len == 0u) {
        if (out_cap < 2u) return false;
        path[0] = '/';
        path[1] = '\0';
        cur_len = 1u;
    }

    if (cur_len > 1u) {
        if (cur_len + 1u >= out_cap) return false;
        path[cur_len++] = '/';
        path[cur_len] = '\0';
    }

    if (cur_len + comp_len + 1u > out_cap) return false;
    memcpy(path + cur_len, comp, comp_len);
    path[cur_len + comp_len] = '\0';
    return true;
}

static bool shell_path_has_slash(const char* s) {
    if (!s) return false;
    while (*s) {
        if (*s == '/') return true;
        s++;
    }
    return false;
}

static bool normalize_shell_path(const char* in, char* out, uint32_t out_cap) {
    const char* cur = NULL;

    if (!in || !*in || !out || out_cap < 2u) return false;

    if (in[0] == '/') {
        out[0] = '/';
        out[1] = '\0';
        cur = in + 1;
    } else {
        uint32_t cwd_len = (uint32_t)strlen(g_shell_cwd);
        if (cwd_len + 1u > out_cap) return false;
        memcpy(out, g_shell_cwd, cwd_len + 1u);
        cur = in;
    }

    while (*cur) {
        const char* comp = NULL;
        uint32_t comp_len = 0;

        while (*cur == '/') cur++;
        if (*cur == '\0') break;

        comp = cur;
        while (*cur && *cur != '/') cur++;
        comp_len = (uint32_t)(cur - comp);

        if (!shell_path_append_component(out, out_cap, comp, comp_len)) {
            return false;
        }
    }

    return true;
}

static bool resolve_program_path(const char* in, char* out, uint32_t out_cap) {
    vnode_t* vn = NULL;
    const char* root_prefix = "/";
    const char* bin_prefix = "/bin/";
    uint32_t in_len = 0;
    uint32_t prefix_len = 0;

    if (!in || !*in || !out || out_cap < 2u) return false;

    if (in[0] == '/' || shell_path_has_slash(in)) {
        return normalize_shell_path(in, out, out_cap);
    }

    in_len = (uint32_t)strlen(in);

    prefix_len = (uint32_t)strlen(bin_prefix);
    if (prefix_len + in_len + 1u <= out_cap) {
        memcpy(out, bin_prefix, prefix_len);
        memcpy(out + prefix_len, in, in_len + 1u);
        if (vfs_resolve(out, &vn) && vn) {
            if (vn->type == VNODE_FILE) {
                vfs_vnode_put(vn);
                return true;
            }
            vfs_vnode_put(vn);
        }
    }

    prefix_len = (uint32_t)strlen(root_prefix);
    if (prefix_len + in_len + 1u <= out_cap) {
        memcpy(out, root_prefix, prefix_len);
        memcpy(out + prefix_len, in, in_len + 1u);
        if (vfs_resolve(out, &vn) && vn) {
            if (vn->type == VNODE_FILE) {
                vfs_vnode_put(vn);
                return true;
            }
            vfs_vnode_put(vn);
        }
    }

    return false;
}

static const char* shell_path_basename(const char* path) {
    const char* last = NULL;

    if (!path || !*path) {
        return NULL;
    }

    last = path;
    while (*path) {
        if (*path == '/' && path[1] != '\0') {
            last = path + 1;
        }
        path++;
    }

    return last;
}

static bool build_kxe_argv(const char* argv0,
                           const char* args,
                           char storage[KXE_MAX_ARGC][KXE_ARG_MAX],
                           const char* argv[KXE_MAX_ARGC],
                           uint64_t* out_argc) {
    const char* cur = trim_spaces(args);
    uint64_t argc = 0;
    uint32_t len = 0;

    if (!argv0 || !*argv0 || !storage || !argv || !out_argc) {
        return false;
    }

    len = (uint32_t)strlen(argv0);
    if (len + 1u > KXE_ARG_MAX) {
        return false;
    }
    memcpy(storage[argc], argv0, len + 1u);
    argv[argc] = storage[argc];
    argc++;

    while (cur && *cur) {
        if (argc >= KXE_MAX_ARGC) {
            return false;
        }
        if (!copy_next_token(&cur, storage[argc], KXE_ARG_MAX)) {
            return false;
        }
        argv[argc] = storage[argc];
        argc++;
        cur = trim_spaces(cur);
    }

    *out_argc = argc;
    return true;
}

static bool resolve_copy_target_path(const char* src_path,
                                     const char* dst_path,
                                     char* out,
                                     uint32_t out_cap) {
    vnode_t* vn = NULL;
    const char* base = NULL;
    uint32_t dst_len = 0;
    uint32_t base_len = 0;

    if (!src_path || !dst_path || !out || out_cap < 2u) {
        return false;
    }

    if (!(vfs_resolve(dst_path, &vn) && vn && vn->type == VNODE_DIR)) {
        if (vn) {
            vfs_vnode_put(vn);
        }
        dst_len = (uint32_t)strlen(dst_path);
        if (dst_len + 1u > out_cap) {
            return false;
        }
        memcpy(out, dst_path, dst_len + 1u);
        return true;
    }
    vfs_vnode_put(vn);

    base = shell_path_basename(src_path);
    if (!base || !*base) {
        return false;
    }

    dst_len = (uint32_t)strlen(dst_path);
    base_len = (uint32_t)strlen(base);

    if (strcmp(dst_path, "/") == 0) {
        if (1u + base_len + 1u > out_cap) {
            return false;
        }
        out[0] = '/';
        memcpy(out + 1u, base, base_len + 1u);
        return true;
    }

    if (dst_len + 1u + base_len + 1u > out_cap) {
        return false;
    }

    memcpy(out, dst_path, dst_len);
    out[dst_len] = '/';
    memcpy(out + dst_len + 1u, base, base_len + 1u);
    return true;
}

static bool ensure_writable_vnode(const char* path, vnode_t** out) {
    vnode_t* vn = NULL;

    if (!out) return false;
    *out = NULL;

    if (vfs_resolve(path, &vn) && vn) {
        if (!vn->ops || !vn->ops->write || !vn->ops->truncate) {
            vfs_vnode_put(vn);
            return false;
        }
        if (!vn->ops->truncate(vn, 0)) {
            vfs_vnode_put(vn);
            return false;
        }
        *out = vn;
        return true;
    }

    if (!vfs_create(path, 0644u, &vn) || !vn) {
        return false;
    }

    if (!vn->ops || !vn->ops->write) {
        vfs_vnode_put(vn);
        return false;
    }

    *out = vn;
    return true;
}

static bool copy_file_vfs(struct limine_framebuffer* fb,
                          const char* src_path,
                          const char* dst_path,
                          bool unlink_source) {
    vnode_t* src = NULL;
    vnode_t* dst = NULL;
    vnode_t* check = NULL;
    uint8_t* tmp = NULL;
    uint64_t src_off = 0;
    uint64_t dst_off = 0;
    uint64_t expected_size = 0;

    if (!src_path || !dst_path || strcmp(src_path, dst_path) == 0) {
        print(fb, unlink_source ? "mv: invalid paths\n" : "cp: invalid paths\n");
        return false;
    }

    if (!vfs_resolve(src_path, &src) || !src) {
        print(fb, unlink_source ? "mv: source not found\n" : "cp: source not found\n");
        return false;
    }

    if (src->type != VNODE_FILE || !src->ops || !src->ops->read) {
        print(fb, unlink_source ? "mv: only regular files supported\n" : "cp: only regular files supported\n");
        vfs_vnode_put(src);
        return false;
    }
    expected_size = src->size;

    if (!ensure_writable_vnode(dst_path, &dst) || !dst) {
        print(fb, unlink_source ? "mv: destination open failed\n" : "cp: destination open failed\n");
        vfs_vnode_put(src);
        return false;
    }

    tmp = (uint8_t*)kmalloc(512u);
    if (!tmp) {
        print(fb, "copy: oom\n");
        vfs_vnode_put(src);
        vfs_vnode_put(dst);
        return false;
    }

    while (1) {
        int64_t n = src->ops->read(src, src_off, tmp, 512u);
        if (n < 0) {
            print(fb, unlink_source ? "mv: read failed\n" : "cp: read failed\n");
            kfree(tmp);
            vfs_vnode_put(src);
            vfs_vnode_put(dst);
            return false;
        }
        if (n == 0) break;
        if (!dst->ops || !dst->ops->write || dst->ops->write(dst, dst_off, tmp, (uint64_t)n) != n) {
            print(fb, unlink_source ? "mv: write failed\n" : "cp: write failed\n");
            kfree(tmp);
            vfs_vnode_put(src);
            vfs_vnode_put(dst);
            return false;
        }
        src_off += (uint64_t)n;
        dst_off += (uint64_t)n;
    }

    kfree(tmp);
    vfs_vnode_put(src);
    vfs_vnode_put(dst);

    if (!vfs_resolve(dst_path, &check) || !check || check->size != expected_size) {
        if (check) {
            vfs_vnode_put(check);
        }
        print(fb, unlink_source ? "mv: verify failed; destination size mismatch\n"
                                : "cp: verify failed; destination size mismatch\n");
        (void)vfs_unlink(dst_path);
        return false;
    }
    vfs_vnode_put(check);

    if (unlink_source && !vfs_unlink(src_path)) {
        print(fb, "mv: unlink failed\n");
        return false;
    }

    return true;
}

static bool path_is_directory(const char* path) {
    vnode_t* vn = NULL;
    bool is_dir = false;

    if (vfs_resolve(path, &vn) && vn) {
        is_dir = (vn->type == VNODE_DIR);
        vfs_vnode_put(vn);
    }

    return is_dir;
}

static bool shell_pattern_has_wildcards(const char* s) {
    if (!s) {
        return false;
    }

    while (*s) {
        if (*s == '*' || *s == '?') {
            return true;
        }
        s++;
    }

    return false;
}

static bool shell_pattern_match(const char* pattern, const char* text) {
    if (!pattern || !text) {
        return false;
    }

    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') {
                pattern++;
            }
            if (*pattern == '\0') {
                return true;
            }
            while (*text) {
                if (shell_pattern_match(pattern, text)) {
                    return true;
                }
                text++;
            }
            return false;
        }

        if (*pattern == '?') {
            if (*text == '\0') {
                return false;
            }
            pattern++;
            text++;
            continue;
        }

        if (*pattern != *text) {
            return false;
        }
        pattern++;
        text++;
    }

    return *text == '\0';
}

static bool split_copy_glob(const char* raw,
                            char* dir_raw,
                            uint32_t dir_cap,
                            char* pattern,
                            uint32_t pattern_cap) {
    const char* last_slash = NULL;
    uint32_t dir_len = 0;
    uint32_t pat_len = 0;

    if (!raw || !dir_raw || !pattern || dir_cap < 2u || pattern_cap < 2u) {
        return false;
    }

    for (const char* p = raw; *p; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }

    if (!last_slash) {
        dir_raw[0] = '.';
        dir_raw[1] = '\0';
        pat_len = (uint32_t)strlen(raw);
        if (pat_len + 1u > pattern_cap) {
            return false;
        }
        memcpy(pattern, raw, pat_len + 1u);
        return true;
    }

    dir_len = (uint32_t)(last_slash - raw);
    if (dir_len == 0) {
        dir_len = 1;
    }
    if (dir_len + 1u > dir_cap) {
        return false;
    }

    memcpy(dir_raw, raw, dir_len);
    dir_raw[dir_len] = '\0';

    pat_len = (uint32_t)strlen(last_slash + 1);
    if (pat_len == 0 || pat_len + 1u > pattern_cap) {
        return false;
    }

    memcpy(pattern, last_slash + 1, pat_len + 1u);
    return true;
}

typedef struct {
    struct limine_framebuffer* fb;
    const char* src_dir;
    const char* pattern;
    const char* dst_dir;
    uint32_t copied;
    bool failed;
} copy_glob_ctx_t;

static bool copy_glob_readdir_cb(const char* name, uint32_t ino, void* user) {
    copy_glob_ctx_t* ctx = (copy_glob_ctx_t*)user;
    char src[256];
    char target[256];
    uint32_t dir_len = 0;
    uint32_t name_len = 0;

    (void)ino;

    if (!ctx || !name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }

    if (!shell_pattern_match(ctx->pattern, name)) {
        return true;
    }

    dir_len = (uint32_t)strlen(ctx->src_dir);
    name_len = (uint32_t)strlen(name);

    if (strcmp(ctx->src_dir, "/") == 0) {
        if (1u + name_len + 1u > sizeof(src)) {
            ctx->failed = true;
            return false;
        }
        src[0] = '/';
        memcpy(src + 1u, name, name_len + 1u);
    } else {
        if (dir_len + 1u + name_len + 1u > sizeof(src)) {
            ctx->failed = true;
            return false;
        }
        memcpy(src, ctx->src_dir, dir_len);
        src[dir_len] = '/';
        memcpy(src + dir_len + 1u, name, name_len + 1u);
    }

    if (!resolve_copy_target_path(src, ctx->dst_dir, target, sizeof(target))) {
        print(ctx->fb, "cp: invalid destination\n");
        ctx->failed = true;
        return false;
    }

    vnode_t* matched = NULL;
    if (vfs_resolve(src, &matched) && matched) {
        bool is_file = (matched->type == VNODE_FILE);
        vfs_vnode_put(matched);
        if (!is_file) {
            return true;
        }
    }

    if (!copy_file_vfs(ctx->fb, src, target, false)) {
        ctx->failed = true;
        return false;
    }

    ctx->copied++;
    return true;
}

static block_device_t* resolve_mount_device_spec(const char* spec) {
    return resolve_block_device_spec(spec, true);
}

static void cmd_mount(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char dev_buf[64];
    char path_buf[256];
    char target_path[256];
    bool ok = false;
    const char* mounted_path = "/";

    if (cur && *cur) {
        block_device_t* dev = NULL;

        if (!copy_next_token(&cur, dev_buf, sizeof(dev_buf))) {
            print(fb, "Usage: mount [device [path]]\n");
            return;
        }

        dev = resolve_mount_device_spec(dev_buf);
        if (!dev) {
            (void)block_rescan();
            dev = resolve_mount_device_spec(dev_buf);
        }
        if (!dev) {
            print(fb, "mount: invalid device or partition\n");
            return;
        }

        cur = trim_spaces(cur);
        if (*cur == '\0') {
            ok = vfs_root_mount() ? vfs_remount_root_dev(dev) : vfs_mount_root_dev(dev);
        } else {
            if (!copy_next_token(&cur, path_buf, sizeof(path_buf)) ||
                !normalize_shell_path(path_buf, target_path, sizeof(target_path)) ||
                *trim_spaces(cur) != '\0') {
                print(fb, "Usage: mount [device [path]]\n");
                return;
            }

            mounted_path = target_path;
            if (strcmp(target_path, "/") == 0) {
                ok = vfs_root_mount() ? vfs_remount_root_dev(dev) : vfs_mount_root_dev(dev);
            } else {
                ok = vfs_mount_dev(target_path, dev);
            }
        }
    } else {
        ok = vfs_mount_root_auto();
    }

    if (!ok) {
        print(fb, "mount: FAILED (see logs)\n");
        return;
    }

    vfs_mount_t* m = vfs_mount_at(mounted_path);
    if (!m) {
        print(fb, "mount: FAILED (no mount)\n");
        return;
    }

    print(fb, "mount: OK (path=");
    print(fb, m->mount_path[0] ? m->mount_path : "?");
    print(fb, ", fs=");
    print(fb, m->fs_name ? m->fs_name : "?");
    print(fb, ", dev=");
    print(fb, m->dev && m->dev->name ? m->dev->name : "?");
    print(fb, m->readonly ? ", ro)\n" : ", rw)\n");
}

static void cmd_mkfs_kifs(struct limine_framebuffer* fb, const char* args) {
    char dev_buf[64];
    char opt_buf[64];
    bool create_kiwios_root = false;
    bool mounted_root = false;
    uint32_t inodes = 1024;

    args = trim_spaces(args);
    if (!copy_next_token(&args, dev_buf, sizeof(dev_buf))) {
        print(fb, "Usage: mkfs.kifs <device> [inodes] [--kiwios-root|--minimal]\n");
        return;
    }

    while (copy_next_token(&args, opt_buf, sizeof(opt_buf))) {
        if (strcmp(opt_buf, "--kiwios-root") == 0) {
            create_kiwios_root = true;
        } else if (strcmp(opt_buf, "--minimal") == 0) {
            create_kiwios_root = false;
        } else if (!parse_u32(opt_buf, &inodes)) {
            print(fb, "Usage: mkfs.kifs <device> [inodes] [--kiwios-root|--minimal]\n");
            return;
        }
    }

    block_device_t* dev = resolve_block_device_spec(dev_buf, true);
    if (!dev) {
        print(fb, "mkfs.kifs: invalid device\n");
        return;
    }

    vfs_mount_t* root = vfs_root_mount();
    mounted_root = root && root->dev == dev;
    if (mounted_root) {
        print(fb, "mkfs.kifs: warning: formatting current root; root will be remounted\n");
    }

    print(fb, "mkfs.kifs: formatting (THIS DESTROYS ALL DATA)...\n");
    if (!kifs_mkfs_ex(dev, inodes, create_kiwios_root)) {
        print(fb, "mkfs.kifs: FAILED (see logs)\n");
        return;
    }
    print(fb, "mkfs.kifs: OK\n");
    if (create_kiwios_root) {
        print(fb, "mkfs.kifs: created /bin /dev /mnt /home /tmp; install /bin/init before userspace boot\n");
    }

    if (mounted_root || !vfs_root_mount()) {
        if (vfs_remount_root_dev(dev)) {
            print(fb, "mkfs.kifs: root mounted at /\n");
        } else {
            print(fb, "mkfs.kifs: formatted, but root remount failed\n");
        }
    }
}

static void cmd_kifsbmdump(struct limine_framebuffer* fb, const char* args) {
    uint64_t nbits64 = 128;
    if (args && *args) {
        if (!parse_u64(args, &nbits64)) {
            print(fb, "Usage: kifsbmdump [nbits]\n");
            return;
        }
    }
    if (nbits64 == 0 || nbits64 > 2048) {
        print(fb, "kifsbmdump: nbits must be 1..2048\n");
        return;
    }
    uint32_t nbits = (uint32_t)nbits64;

    vfs_mount_t* m = vfs_root_mount();
    if (!m) {
        print(fb, "kifsbmdump: no root filesystem mounted\n");
        return;
    }
    if (!m->fs_name || strcmp(m->fs_name, "kifs") != 0) {
        print(fb, "kifsbmdump: root filesystem is not KiFS\n");
        return;
    }

    uint8_t* blk_bits = (uint8_t*)kmalloc(nbits);
    uint8_t* ino_bits = (uint8_t*)kmalloc(nbits);
    if (!blk_bits || !ino_bits) {
        if (blk_bits) kfree(blk_bits);
        if (ino_bits) kfree(ino_bits);
        print(fb, "kifsbmdump: out of memory\n");
        return;
    }

    if (!kifs_debug_get_bitmap_bits(m, false, 0, nbits, blk_bits) ||
        !kifs_debug_get_bitmap_bits(m, true, 0, nbits, ino_bits)) {
        kfree(blk_bits);
        kfree(ino_bits);
        print(fb, "kifsbmdump: failed to read/validate bitmap blocks\n");
        return;
    }

    print(fb, "Block bitmap (first bits):\n");
    for (uint32_t i = 0; i < nbits; i++) {
        putc_fb(fb, blk_bits[i] ? '1' : '0');
        if ((i % 64u) == 63u) putc_fb(fb, '\n');
        else if ((i % 8u) == 7u) putc_fb(fb, ' ');
    }
    if ((nbits % 64u) != 0) putc_fb(fb, '\n');

    print(fb, "Inode bitmap (first bits):\n");
    for (uint32_t i = 0; i < nbits; i++) {
        putc_fb(fb, ino_bits[i] ? '1' : '0');
        if ((i % 64u) == 63u) putc_fb(fb, '\n');
        else if ((i % 8u) == 7u) putc_fb(fb, ' ');
    }
    if ((nbits % 64u) != 0) putc_fb(fb, '\n');

    kfree(blk_bits);
    kfree(ino_bits);
}

static void cmd_kifs_sb(struct limine_framebuffer* fb) {
    vfs_mount_t* m = vfs_root_mount();
    if (!m) {
        print(fb, "kifs sb: no root filesystem mounted\n");
        return;
    }
    if (!m->fs_name || strcmp(m->fs_name, "kifs") != 0) {
        print(fb, "kifs sb: root filesystem is not KiFS\n");
        return;
    }
    kifs_superblock_t sb;
    if (!kifs_debug_get_superblock(m, &sb)) {
        print(fb, "kifs sb: failed to read superblock from mount\n");
        return;
    }

    print(fb, "KiFS superblock (selected):\n");
    print(fb, "  sb_seq="); print_u64(fb, sb.sb_seq); print(fb, sb.dirty ? " (dirty)\n" : " (clean)\n");
    print(fb, "  total_blocks="); print_u64(fb, sb.total_blocks);
    print(fb, "  usable_blocks="); print_u64(fb, sb.usable_blocks);
    print(fb, "  block_size="); print_u64(fb, sb.block_size); print(fb, "\n");

    print(fb, "Layout (blocks):\n");
    print(fb, "  journal: start="); print_u64(fb, sb.journal_start); print(fb, " count="); print_u64(fb, sb.journal_blocks); print(fb, "\n");
    print(fb, "  block_bitmap: start="); print_u64(fb, sb.block_bitmap_start); print(fb, " count="); print_u64(fb, sb.block_bitmap_blocks); print(fb, "\n");
    print(fb, "  inode_bitmap: start="); print_u64(fb, sb.inode_bitmap_start); print(fb, " count="); print_u64(fb, sb.inode_bitmap_blocks); print(fb, "\n");
    print(fb, "  inode_table:  start="); print_u64(fb, sb.inode_table_start); print(fb, " count="); print_u64(fb, sb.inode_table_blocks); print(fb, "\n");
    print(fb, "  data:        start="); print_u64(fb, sb.data_start); print(fb, " count="); print_u64(fb, sb.data_blocks); print(fb, "\n");

    print(fb, "Inodes:\n");
    print(fb, "  inode_count="); print_u64(fb, sb.inode_count); print(fb, "\n");
    print(fb, "  root_ino="); print_u64(fb, sb.root_ino);
    print(fb, "  orphan_ino="); print_u64(fb, sb.orphan_ino); print(fb, "\n");
}

static void cmd_kifs(struct limine_framebuffer* fb, const char* args) {
    args = trim_spaces(args);
    if (!args || !*args) {
        print(fb, "Usage: kifs <subcmd> ...\n");
        print(fb, "  kifs sb\n");
        print(fb, "  kifs bmdump [nbits]\n");
        print(fb, "  kifs mkfs <device> [inodes] [--kiwios-root|--minimal]\n");
        print(fb, "  kifs mount [device [path]]\n");
        return;
    }

    // extract first token
    char sub[16];
    uint32_t i=0;
    while (args[i] && args[i] != ' ' && i < (sizeof(sub)-1)) { sub[i]=args[i]; i++; }
    sub[i]='\0';
    const char* rest = skip_token(args);

    if (strcmp(sub, "sb") == 0) {
        cmd_kifs_sb(fb);
        return;
    }
    if (strcmp(sub, "bmdump") == 0) {
        cmd_kifsbmdump(fb, rest);
        return;
    }
    if (strcmp(sub, "mkfs") == 0) {
        cmd_mkfs_kifs(fb, rest);
        return;
    }
    if (strcmp(sub, "mount") == 0) {
        cmd_mount(fb, rest);
        return;
    }

    print(fb, "kifs: unknown subcommand. Try: kifs sb | bmdump | mkfs | mount\n");
}

typedef struct {
    struct limine_framebuffer* fb;
} ls_ctx_t;

static bool ls_cb(const char* name, uint32_t ino, void* user) {
    (void)ino;
    ls_ctx_t* ctx = (ls_ctx_t*)user;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }
    print(ctx->fb, name);
    print(ctx->fb, "\n");
    return true;
}

static void cmd_pwd(struct limine_framebuffer* fb, const char* args) {
    if (args && *trim_spaces(args) != '\0') {
        print(fb, "Usage: pwd\n");
        return;
    }

    print(fb, g_shell_cwd);
    print(fb, "\n");
}

static void cmd_cd(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];
    vnode_t* vn = NULL;

    if (!cur || !*cur) {
        path[0] = '/';
        path[1] = '\0';
    } else if (!copy_next_token(&cur, raw, sizeof(raw)) ||
               !normalize_shell_path(raw, path, sizeof(path)) ||
               *trim_spaces(cur) != '\0') {
        print(fb, "Usage: cd [path]\n");
        return;
    }

    if (!vfs_resolve(path, &vn) || !vn || vn->type != VNODE_DIR) {
        if (vn) vfs_vnode_put(vn);
        print(fb, "cd: not a directory\n");
        return;
    }

    vfs_vnode_put(vn);
    memcpy(g_shell_cwd, path, strlen(path) + 1u);
}

static void cmd_ls(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];
    ls_ctx_t ctx = { .fb = fb };

    if (!cur || !*cur) {
        memcpy(path, g_shell_cwd, strlen(g_shell_cwd) + 1u);
    } else if (!copy_next_token(&cur, raw, sizeof(raw)) ||
               !normalize_shell_path(raw, path, sizeof(path)) ||
               *trim_spaces(cur) != '\0') {
        print(fb, "Usage: ls [path]\n");
        return;
    }

    vnode_t* vn = NULL;
    if (!vfs_resolve(path, &vn) || !vn) {
        print(fb, "ls: not found\n");
        return;
    }

    if (vn->type != VNODE_DIR) {
        print(fb, "ls: not a directory\n");
        vfs_vnode_put(vn);
        return;
    }
    vfs_vnode_put(vn);

    if (!vfs_readdir(path, ls_cb, &ctx)) {
        print(fb, "ls: no readdir\n");
    }
}

static void cmd_stat(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];

    if (!copy_next_token(&cur, raw, sizeof(raw)) ||
        !normalize_shell_path(raw, path, sizeof(path)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: stat <path>\n");
        return;
    }

    vnode_t* vn = NULL;
    if (!vfs_resolve(path, &vn) || !vn) {
        print(fb, "stat: not found\n");
        return;
    }

    if (!vn->ops || !vn->ops->stat) {
        print(fb, "stat: no stat\n");
        vfs_vnode_put(vn);
        return;
    }

    vfs_stat_t st;
    if (!vn->ops->stat(vn, &st)) {
        print(fb, "stat: failed\n");
        vfs_vnode_put(vn);
        return;
    }

    print(fb, "ino=");
    print_u32(fb, st.ino);
    print(fb, " type=");
    print(fb, st.type == VNODE_DIR ? "dir" : "file");
    print(fb, " size=");
    print_u64(fb, st.size);
    print(fb, " links=");
    print_u32(fb, st.link_count);
    print(fb, "\n");

    vfs_vnode_put(vn);
}

static void cmd_cat(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];

    if (!copy_next_token(&cur, raw, sizeof(raw)) ||
        !normalize_shell_path(raw, path, sizeof(path)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: cat <path>\n");
        return;
    }

    vnode_t* vn = NULL;
    if (!vfs_resolve(path, &vn) || !vn) {
        print(fb, "cat: not found\n");
        return;
    }

    if (!vn->ops || !vn->ops->read) {
        print(fb, "cat: no read\n");
        vfs_vnode_put(vn);
        return;
    }

    uint8_t* tmp = (uint8_t*)kmalloc(512);
    if (!tmp) {
        print(fb, "cat: oom\n");
        vfs_vnode_put(vn);
        return;
    }

    uint64_t off = 0;
    while (1) {
        int64_t n = vn->ops->read(vn, off, tmp, 512);
        if (n < 0) {
            print(fb, "cat: read error\n");
            break;
        }
        if (n == 0) break;
        for (int64_t i = 0; i < n; i++) {
            putc_fb(fb, (char)tmp[i]);
        }
        off += (uint64_t)n;
    }

    putc_fb(fb, '\n');
    kfree(tmp);
    vfs_vnode_put(vn);
}

static void cmd_touch(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];
    vnode_t* vn = NULL;

    if (!copy_next_token(&cur, raw, sizeof(raw)) ||
        !normalize_shell_path(raw, path, sizeof(path)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: touch <path>\n");
        return;
    }

    if (vfs_resolve(path, &vn) && vn) {
        if (vn->type != VNODE_FILE) {
            print(fb, "touch: path is not a file\n");
        }
        vfs_vnode_put(vn);
        return;
    }

    if (!vfs_create(path, 0644u, &vn) || !vn) {
        print(fb, "touch: create failed\n");
        return;
    }

    vfs_vnode_put(vn);
}

static void cmd_mkdir_vfs(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];

    if (!copy_next_token(&cur, raw, sizeof(raw)) ||
        !normalize_shell_path(raw, path, sizeof(path)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: mkdir <path>\n");
        return;
    }

    if (!vfs_mkdir(path, 0755u)) {
        print(fb, "mkdir: failed\n");
    }
}

static void cmd_rm_vfs(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];
    char path[256];

    if (!copy_next_token(&cur, raw, sizeof(raw)) ||
        !normalize_shell_path(raw, path, sizeof(path)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: rm <path>\n");
        return;
    }

    if (!vfs_unlink(path)) {
        print(fb, "rm: failed\n");
    }
}

static void cmd_cp_vfs(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char src_raw[256];
    char dst_raw[256];
    char dir_raw[256];
    char pattern[128];
    char src_dir[256];
    char src[256];
    char dst[256];
    char target[256];

    if (!copy_next_token(&cur, src_raw, sizeof(src_raw)) ||
        !copy_next_token(&cur, dst_raw, sizeof(dst_raw)) ||
        !normalize_shell_path(dst_raw, dst, sizeof(dst)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: cp <src> <dst>\n");
        return;
    }

    if (shell_pattern_has_wildcards(src_raw)) {
        copy_glob_ctx_t ctx;

        if (!split_copy_glob(src_raw, dir_raw, sizeof(dir_raw), pattern, sizeof(pattern)) ||
            !normalize_shell_path(dir_raw, src_dir, sizeof(src_dir))) {
            print(fb, "cp: invalid source pattern\n");
            return;
        }

        if (!path_is_directory(dst)) {
            print(fb, "cp: wildcard destination must be a directory\n");
            return;
        }

        ctx.fb = fb;
        ctx.src_dir = src_dir;
        ctx.pattern = pattern;
        ctx.dst_dir = dst;
        ctx.copied = 0;
        ctx.failed = false;

        if (!vfs_readdir(src_dir, copy_glob_readdir_cb, &ctx)) {
            if (!ctx.failed) {
                print(fb, "cp: source directory not found\n");
            }
            return;
        }

        if (!ctx.failed && ctx.copied == 0) {
            print(fb, "cp: source not found\n");
        }
        return;
    }

    if (!normalize_shell_path(src_raw, src, sizeof(src))) {
        print(fb, "cp: invalid source\n");
        return;
    }

    if (!resolve_copy_target_path(src, dst, target, sizeof(target))) {
        print(fb, "cp: invalid destination\n");
        return;
    }

    (void)copy_file_vfs(fb, src, target, false);
}

static void cmd_mv_vfs(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char src_raw[256];
    char dst_raw[256];
    char src[256];
    char dst[256];
    char target[256];

    if (!copy_next_token(&cur, src_raw, sizeof(src_raw)) ||
        !copy_next_token(&cur, dst_raw, sizeof(dst_raw)) ||
        !normalize_shell_path(src_raw, src, sizeof(src)) ||
        !normalize_shell_path(dst_raw, dst, sizeof(dst)) ||
        *trim_spaces(cur) != '\0') {
        print(fb, "Usage: mv <src> <dst>\n");
        return;
    }

    if (!resolve_copy_target_path(src, dst, target, sizeof(target))) {
        print(fb, "mv: invalid destination\n");
        return;
    }

    (void)copy_file_vfs(fb, src, target, true);
}

static bool run_kxe_command(struct limine_framebuffer* fb,
                            const char* raw,
                            const char* args,
                            bool quiet_not_found) {
    char path[256];
    char argv_storage[KXE_MAX_ARGC][KXE_ARG_MAX];
    const char* argv[KXE_MAX_ARGC];
    uint64_t argc = 0;
    process_t* proc = NULL;

    if (!raw || !*raw) {
        return false;
    }

    if (!resolve_program_path(raw, path, sizeof(path))) {
        if (!quiet_not_found) {
            print(fb, "exec: program not found\n");
        }
        return false;
    }

    if (!build_kxe_argv(raw, args, argv_storage, argv, &argc)) {
        print(fb, "exec: too many arguments or argument too long\n");
        return true;
    }

    proc = kxe_load_argv(path, argc, argv);
    if (!proc) {
        print(fb, "exec: load failed\n");
        return true;
    }

    print(fb, "exec: scheduling process...\n");
    scheduler_run(proc);
    return true;
}

static void cmd_exec(struct limine_framebuffer* fb, const char* args) {
    const char* cur = trim_spaces(args);
    char raw[256];

    if (!copy_next_token(&cur, raw, sizeof(raw))) {
        print(fb, "Usage: exec <path> [args...]\n");
        return;
    }

    (void)run_kxe_command(fb, raw, cur, false);
}

static void cmd_usertest(struct limine_framebuffer* fb) {
    usertest_run(fb);
}

// -------- Unknown --------

static void cmd_unknown(struct limine_framebuffer *fb, const char *cmd) {
    print(fb, "Unknown command: ");
    print(fb, cmd);
    print(fb, "\n");
    print(fb, "Type 'help' for available commands\n");
}

// ================= Command dispatch =================
typedef void (*cmd_func_t)(struct limine_framebuffer *fb);

typedef enum {
    COMMAND_NO_ARGS,
    COMMAND_TAKES_ARGS
} command_arity;

struct command {
    const char *name;
    cmd_func_t func;
    command_arity arity;
};

static void execute_command(struct limine_framebuffer *fb, char *input) {
    // Skip leading spaces
    while (*input == ' ') input++;

    // Empty command
    if (*input == '\0') return;

    // Find end of command word
    char *args = input;
    while (*args && *args != ' ') args++;

    // Split command and args
    if (*args) {
        *args = '\0'; // null terminate command
        args++; // point to arguments
        while (*args == ' ') args++; // skip spaces
    }

    if (strcmp(input, "help") == 0) {
        cmd_help(fb);
        return;
    }

    if (strcmp(input, "clear") == 0) {
        cmd_clear(fb);
        return;
    }

    if (strcmp(input, "echo") == 0) {
        cmd_echo(fb, args);
        return;
    }

    if (strcmp(input, "about") == 0) {
        cmd_about(fb);
        return;
    }

    if (strcmp(input, "crash") == 0) {
        cmd_crash(fb, args);
        return;
    }

    if (strcmp(input, "meminfo") == 0) {
        cmd_meminfo(fb);
        return;
    }

    if (strcmp(input, "memtest") == 0) {
        cmd_memtest(fb);
        return;
    }

    if (strcmp(input, "vmtest") == 0) {
        cmd_vmtest(fb);
        return;
    }

    if (strcmp(input, "heaptest") == 0) {
        cmd_heaptest(fb);
        return;
    }

    if (strcmp(input, "fbinfo") == 0) {
        cmd_fbinfo(fb);
        return;
    }

    if (strcmp(input, "scale") == 0) {
        cmd_scale(fb, args);
        return;
    }

    if (strcmp(input, "rawread") == 0) {
        cmd_rawread(fb, args);
        return;
    }

    if (strcmp(input, "rawwrite") == 0) {
        cmd_rawwrite(fb, args);
        return;
    }

    if (strcmp(input, "disktest") == 0) {
        cmd_disktest(fb);
        return;
    }

    if (strcmp(input, "rawflush") == 0) {
        cmd_rawflush(fb);
        return;
    }

    if (strcmp(input, "partlist") == 0) {
        cmd_partlist(fb);
        return;
    }

    if (strcmp(input, "rescan") == 0) {
        cmd_rescan(fb);
        return;
    }

    if (strcmp(input, "usb") == 0) {
        cmd_usb(fb);
        return;
    }

    if (strcmp(input, "acpi") == 0) {
        cmd_acpi(fb);
        return;
    }

    if (strcmp(input, "apic") == 0) {
        cmd_apic(fb);
        return;
    }

    if (strcmp(input, "hpet") == 0) {
        cmd_hpet(fb);
        return;
    }

    if (strcmp(input, "reboot") == 0) {
        cmd_reboot(fb);
        return;
    }

    if (strcmp(input, "poweroff") == 0 || strcmp(input, "shutdown") == 0) {
        cmd_poweroff(fb);
        return;
    }

    if (strcmp(input, "diskreadp") == 0) {
        cmd_diskreadp(fb, args);
        return;
    }

    if (strcmp(input, "diskwritep") == 0) {
        cmd_diskwritep(fb, args);
        return;
    }

    if (strcmp(input, "diskflushp") == 0) {
        cmd_diskflushp(fb, args);
        return;
    }

    if (strcmp(input, "bcachestat") == 0) {
        cmd_bcachestat(fb);
        return;
    }

    if (strcmp(input, "bcacheflush") == 0) {
        cmd_bcacheflush(fb);
        return;
    }

    if (strcmp(input, "bcacheflushp") == 0) {
        cmd_bcacheflushp(fb, args);
        return;
    }

    if (strcmp(input, "mount") == 0) {
        cmd_mount(fb, args);
        return;
    }

    if (strcmp(input, "kifs") == 0) {
        cmd_kifs(fb, args);
        return;
    }

    if (strcmp(input, "mkfs.kifs") == 0) {
        cmd_mkfs_kifs(fb, args);
        return;
    }

    if (strcmp(input, "kifsbmdump") == 0) {
        cmd_kifsbmdump(fb, args);
        return;
    }

    if (strcmp(input, "pwd") == 0) {
        cmd_pwd(fb, args);
        return;
    }

    if (strcmp(input, "cd") == 0) {
        cmd_cd(fb, args);
        return;
    }

    if (strcmp(input, "ls") == 0) {
        cmd_ls(fb, args);
        return;
    }

    if (strcmp(input, "stat") == 0) {
        cmd_stat(fb, args);
        return;
    }

    if (strcmp(input, "cat") == 0) {
        cmd_cat(fb, args);
        return;
    }

    if (strcmp(input, "touch") == 0) {
        cmd_touch(fb, args);
        return;
    }

    if (strcmp(input, "mkdir") == 0) {
        cmd_mkdir_vfs(fb, args);
        return;
    }

    if (strcmp(input, "rm") == 0) {
        cmd_rm_vfs(fb, args);
        return;
    }

    if (strcmp(input, "cp") == 0) {
        cmd_cp_vfs(fb, args);
        return;
    }

    if (strcmp(input, "mv") == 0) {
        cmd_mv_vfs(fb, args);
        return;
    }

    if (strcmp(input, "exec") == 0) {
        cmd_exec(fb, args);
        return;
    }

    if (strcmp(input, "usertest") == 0) {
        cmd_usertest(fb);
        return;
    }

    if (run_kxe_command(fb, input, args, true)) {
        return;
    }

    // Command not found
    cmd_unknown(fb, input);
}

// ================= Input handling =================
#define INPUT_BUFFER_SIZE 256

#define HISTORY_SIZE 32
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_count = 0;   // number of stored entries
static int history_cursor = -1; // -1 = live typing, 0 = newest history, ...
static char history_scratch[INPUT_BUFFER_SIZE];
static int history_scratch_len = 0;

static void history_record(const char *line) {
    if (!line || !*line) return;

    // Avoid duplicate consecutive entries
    if (history_count > 0) {
        const char *last = history[(history_count - 1) % HISTORY_SIZE];
        if (strncmp(last, line, INPUT_BUFFER_SIZE) == 0) return;
    }

    size_t len = strlen(line);
    if (len >= INPUT_BUFFER_SIZE) len = INPUT_BUFFER_SIZE - 1;

    int slot = history_count % HISTORY_SIZE;
    memcpy(history[slot], line, len);
    history[slot][len] = '\0';
    history_count++;
}

static void reset_history_navigation(void) {
    history_cursor = -1;
    history_scratch_len = 0;
}

static const char *history_fetch(int cursor_from_newest) {
    if (cursor_from_newest < 0) return NULL;
    if (cursor_from_newest >= history_count) return NULL;
    int logical = history_count - 1 - cursor_from_newest;
    return history[logical % HISTORY_SIZE];
}

static void replace_input_line(char *buffer, int *len, int *cursor,
                               const char *text) {
    size_t text_len = strlen(text);
    if (text_len >= INPUT_BUFFER_SIZE) {
        text_len = INPUT_BUFFER_SIZE - 1;
    }

    memcpy(buffer, text, text_len);
    buffer[text_len] = '\0';
    *len = (int)text_len;
    *cursor = (int)text_len;
    console_set_input_line("> ", buffer, (uint32_t)(*len), (uint32_t)(*cursor), true);
}

static int shell_getchar_hotplug(void) {
    uint32_t idle_ticks = 0;

    for (;;) {
        int ch = keyboard_getchar_nonblocking();
        if (ch >= 0 ||
            ch == KEY_ARROW_UP || ch == KEY_ARROW_DOWN ||
            ch == KEY_ARROW_LEFT || ch == KEY_ARROW_RIGHT) {
            return ch;
        }

        if (ch == KEY_PAGE_UP) {
            console_page_up();
            continue;
        }
        if (ch == KEY_PAGE_DOWN) {
            console_page_down();
            continue;
        }

        idle_ticks++;
        if (idle_ticks >= 10u) {
            idle_ticks = 0;
            (void)block_poll_hotplug();
        }

        asm volatile("hlt");
    }
}

void shell_loop(struct limine_framebuffer *fb) {
    char input_buffer[INPUT_BUFFER_SIZE];
    int input_len = 0;
    int cursor_pos = 0;
    input_buffer[0] = '\0';
    log_info("shell", "interactive shell started");

    print(fb, "Welcome to kiwiOS!\n");
    print(fb, "Type 'help' for available commands\n\n");
    console_set_input_line("> ", input_buffer, 0, 0, true);

    while (1) {
        int c = shell_getchar_hotplug();
        if (c == KEY_ARROW_UP) {
            if (history_cursor == -1) {
                history_scratch_len = input_len;
                if (history_scratch_len > INPUT_BUFFER_SIZE - 1) history_scratch_len = INPUT_BUFFER_SIZE - 1;
                memcpy(history_scratch, input_buffer, (size_t)history_scratch_len);
                history_scratch[history_scratch_len] = '\0';
            }

            if (history_cursor + 1 < history_count) {
                history_cursor++;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(input_buffer, &input_len, &cursor_pos, entry);
            }
            continue;
        }

        if (c == KEY_ARROW_DOWN) {
            if (history_cursor > 0) {
                history_cursor--;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(input_buffer, &input_len, &cursor_pos, entry);
            } else if (history_cursor == 0) {
                history_cursor = -1;
                replace_input_line(input_buffer, &input_len, &cursor_pos, history_scratch);
            }
            continue;
        }

        if (c == KEY_ARROW_LEFT) {
            if (cursor_pos > 0) {
                cursor_pos--;
                console_set_input_line("> ", input_buffer, (uint32_t)input_len, (uint32_t)cursor_pos, true);
            }
            continue;
        }

        if (c == KEY_ARROW_RIGHT) {
            if (cursor_pos < input_len) {
                cursor_pos++;
                console_set_input_line("> ", input_buffer, (uint32_t)input_len, (uint32_t)cursor_pos, true);
            }
            continue;
        }

        if (c == '\n') {
            // Execute command
            console_set_input_line("> ", input_buffer, (uint32_t)input_len, (uint32_t)input_len, false);
            print(fb, "\n");
            input_buffer[input_len] = '\0';

            if (input_len > 0) {
                history_record(input_buffer);
                execute_command(fb, input_buffer);
            }

            // Reset for next command
            input_len = 0;
            cursor_pos = 0;
            reset_history_navigation();
            console_set_input_line("> ", input_buffer, 0, 0, true);
        } else if (c == '\b') {
            // Backspace
            if (cursor_pos > 0) {
                memmove(&input_buffer[cursor_pos - 1],
                        &input_buffer[cursor_pos],
                        (size_t)(input_len - cursor_pos));
                input_len--;
                cursor_pos--;
                input_buffer[input_len] = '\0';
                console_set_input_line("> ", input_buffer, (uint32_t)input_len, (uint32_t)cursor_pos, true);
            }
        } else if (c >= 0 && input_len < INPUT_BUFFER_SIZE - 1) {
            // Add character to buffer
            memmove(&input_buffer[cursor_pos + 1],
                    &input_buffer[cursor_pos],
                    (size_t)(input_len - cursor_pos));
            input_buffer[cursor_pos] = (char)c;
            input_len++;
            cursor_pos++;
            input_buffer[input_len] = '\0';
            console_set_input_line("> ", input_buffer, (uint32_t)input_len, (uint32_t)cursor_pos, true);
        }
    }
}
