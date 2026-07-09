#include "drivers/block/block.h"
#include "drivers/ahci/ahci.h"
#include "drivers/usb/usb_storage.h"
#include "drivers/pci/pci.h"
#include "core/log.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "libc/string.h"

#define MAX_DISKS 8u
#define MAX_PARTITIONS 64u

typedef enum {
    DISK_BACKEND_NONE = 0,
    DISK_BACKEND_AHCI,
    DISK_BACKEND_USB_STORAGE,
} disk_backend_t;

typedef struct {
    disk_backend_t backend;
    uint32_t backend_index;
    part_table_type_t part_table;
    char name_buf[24];
} disk_ctx_t;

typedef struct {
    block_device_t* parent;
    uint64_t lba_start;
    uint64_t lba_count;
    uint32_t part_number_one_based;

    // For MBR:
    uint8_t  mbr_type;

    // For GPT:
    bool     is_gpt;
    uint8_t  gpt_type_guid[16];
    uint8_t  gpt_part_guid[16];

    char     name_buf[24];
} part_ctx_t;

static block_device_t g_disks[MAX_DISKS];
static disk_ctx_t     g_disk_ctx[MAX_DISKS];
static uint32_t       g_disk_count = 0;
static bool           g_ready = false;
static uint32_t       g_registered_ahci_count = 0;
static uint32_t       g_registered_usb_count = 0;

static block_device_t g_parts[MAX_PARTITIONS];
static part_ctx_t     g_part_ctx[MAX_PARTITIONS];
static uint32_t       g_part_count = 0;

static part_table_type_t g_boot_part_table = PART_TABLE_NONE;

static void sync_disk_presence(block_device_t* dev);
static void sync_all_device_presence(void);

// ---------------- disk device ops ----------------

static bool disk_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    disk_ctx_t* ctx = NULL;

    if (!dev || !dev->ctx) return false;
    sync_disk_presence(dev);
    if (!dev->present) return false;
    ctx = (disk_ctx_t*)dev->ctx;
    switch (ctx->backend) {
        case DISK_BACKEND_AHCI:
            return ahci_disk_read_index(ctx->backend_index, lba, count, buffer);
        case DISK_BACKEND_USB_STORAGE:
            return usb_storage_read(ctx->backend_index, lba, count, buffer);
        default:
            return false;
    }
}

static bool disk_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    disk_ctx_t* ctx = NULL;

    if (!dev || !dev->ctx) return false;
    sync_disk_presence(dev);
    if (!dev->present) return false;
    ctx = (disk_ctx_t*)dev->ctx;
    switch (ctx->backend) {
        case DISK_BACKEND_AHCI:
            return ahci_disk_write_index(ctx->backend_index, lba, count, buffer);
        case DISK_BACKEND_USB_STORAGE:
            return usb_storage_write(ctx->backend_index, lba, count, buffer);
        default:
            return false;
    }
}

static bool disk_flush(block_device_t* dev) {
    disk_ctx_t* ctx = NULL;

    if (!dev || !dev->ctx) return false;
    sync_disk_presence(dev);
    if (!dev->present) return false;
    ctx = (disk_ctx_t*)dev->ctx;
    switch (ctx->backend) {
        case DISK_BACKEND_AHCI:
            return ahci_disk_flush_index(ctx->backend_index);
        case DISK_BACKEND_USB_STORAGE:
            return usb_storage_flush(ctx->backend_index);
        default:
            return false;
    }
}

// ---------------- partition device ops ----------------

static bool part_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    part_ctx_t* p = NULL;

    if (!dev || !dev->ctx) return false;
    p = (part_ctx_t*)dev->ctx;

    if (!p->parent || !p->parent->read) return false;
    if (count == 0) return false;
    sync_disk_presence(p->parent);
    dev->present = p->parent->present;
    if (!dev->present) return false;

    if (p->lba_count != 0) {
        if (lba >= p->lba_count) return false;
        if ((uint64_t)count > (p->lba_count - lba)) return false;
    }

    return p->parent->read(p->parent, p->lba_start + lba, count, buffer);
}

static bool part_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    part_ctx_t* p = NULL;

    if (!dev || !dev->ctx) return false;
    p = (part_ctx_t*)dev->ctx;

    if (!p->parent || !p->parent->write) return false;
    if (count == 0) return false;
    sync_disk_presence(p->parent);
    dev->present = p->parent->present;
    if (!dev->present) return false;

    if (p->lba_count != 0) {
        if (lba >= p->lba_count) return false;
        if ((uint64_t)count > (p->lba_count - lba)) return false;
    }

    return p->parent->write(p->parent, p->lba_start + lba, count, buffer);
}

static bool part_flush(block_device_t* dev) {
    part_ctx_t* p = NULL;

    if (!dev || !dev->ctx) return false;
    p = (part_ctx_t*)dev->ctx;
    if (!p->parent || !p->parent->flush) return true;
    sync_disk_presence(p->parent);
    dev->present = p->parent->present;
    if (!dev->present) return false;
    return p->parent->flush(p->parent);
}

// ---------------- helpers ----------------

static bool guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++) {
        if (g[i] != 0) return false;
    }
    return true;
}

static void make_disk_name(disk_ctx_t* ctx, uint32_t disk_index) {
    char rev[16];
    uint32_t rev_len = 0;
    uint32_t pos = 0;

    if (!ctx) {
        return;
    }

    memset(ctx->name_buf, 0, sizeof(ctx->name_buf));
    memcpy(ctx->name_buf, "disk", 4u);
    pos = 4u;

    do {
        rev[rev_len++] = (char)('0' + (disk_index % 10u));
        disk_index /= 10u;
    } while (disk_index != 0u && rev_len < sizeof(rev));

    while (rev_len > 0u && (pos + 1u) < sizeof(ctx->name_buf)) {
        ctx->name_buf[pos++] = rev[--rev_len];
    }
    ctx->name_buf[pos] = '\0';
}

static void make_part_name(part_ctx_t* ctx, const char* parent_name, uint32_t part_number_one_based) {
    char rev[16];
    uint32_t rev_len = 0;
    uint32_t j = 0;
    const char* base = parent_name ? parent_name : "disk";

    if (!ctx) {
        return;
    }

    memset(ctx->name_buf, 0, sizeof(ctx->name_buf));

    while (base[j] && j < (sizeof(ctx->name_buf) - 1u)) {
        ctx->name_buf[j] = base[j];
        j++;
    }

    if (j < (sizeof(ctx->name_buf) - 1u)) {
        ctx->name_buf[j++] = 'p';
    }

    if (part_number_one_based == 0u) {
        part_number_one_based = 1u;
    }

    do {
        rev[rev_len++] = (char)('0' + (part_number_one_based % 10u));
        part_number_one_based /= 10u;
    } while (part_number_one_based != 0u && rev_len < sizeof(rev));

    while (rev_len > 0u && j < (sizeof(ctx->name_buf) - 1u)) {
        ctx->name_buf[j++] = rev[--rev_len];
    }
    ctx->name_buf[j] = '\0';
}

static bool disk_backend_present(const disk_ctx_t* ctx) {
    if (!ctx) {
        return false;
    }

    switch (ctx->backend) {
        case DISK_BACKEND_AHCI:
            return true;
        case DISK_BACKEND_USB_STORAGE:
            return usb_storage_is_present(ctx->backend_index);
        default:
            return false;
    }
}

static void sync_disk_presence(block_device_t* dev) {
    disk_ctx_t* ctx = NULL;

    if (!dev || !dev->ctx) {
        return;
    }

    ctx = (disk_ctx_t*)dev->ctx;
    dev->present = disk_backend_present(ctx);
}

static void sync_part_presence(block_device_t* dev) {
    part_ctx_t* part = NULL;

    if (!dev || !dev->ctx) {
        return;
    }

    part = (part_ctx_t*)dev->ctx;
    if (!part->parent) {
        dev->present = false;
        return;
    }

    sync_disk_presence(part->parent);
    dev->present = part->parent->present;
    dev->removable = part->parent->removable;
}

static void sync_all_device_presence(void) {
    for (uint32_t i = 0; i < g_disk_count; i++) {
        sync_disk_presence(&g_disks[i]);
    }
    for (uint32_t i = 0; i < g_part_count; i++) {
        sync_part_presence(&g_parts[i]);
    }
}

static block_device_t* register_disk(disk_backend_t backend, uint32_t backend_index) {
    disk_ctx_t* ctx = NULL;
    block_device_t* dev = NULL;
    uint64_t total_sectors = 0;

    if (g_disk_count >= MAX_DISKS) {
        return NULL;
    }

    ctx = &g_disk_ctx[g_disk_count];
    dev = &g_disks[g_disk_count];
    memset(ctx, 0, sizeof(*ctx));
    memset(dev, 0, sizeof(*dev));

    ctx->backend = backend;
    ctx->backend_index = backend_index;
    ctx->part_table = PART_TABLE_NONE;
    make_disk_name(ctx, g_disk_count);

    switch (backend) {
        case DISK_BACKEND_AHCI:
            total_sectors = ahci_disk_total_sectors(backend_index);
            break;
        case DISK_BACKEND_USB_STORAGE:
            total_sectors = usb_storage_total_sectors(backend_index);
            break;
        default:
            return NULL;
    }

    dev->name = ctx->name_buf;
    dev->sector_size = 512u;
    dev->total_sectors = total_sectors;
    dev->present = disk_backend_present(ctx);
    dev->removable = (backend == DISK_BACKEND_USB_STORAGE);
    dev->ctx = ctx;
    dev->read = disk_read;
    dev->write = disk_write;
    dev->flush = disk_flush;

    g_disk_count++;
    return dev;
}

static void register_partition(block_device_t* parent,
                               uint32_t part_number_one_based,
                               uint64_t start,
                               uint64_t count,
                               bool is_gpt,
                               uint8_t mbr_type,
                               const uint8_t gpt_type_guid[16],
                               const uint8_t gpt_part_guid[16]) {
    uint32_t idx = 0;
    part_ctx_t* c = NULL;
    block_device_t* d = NULL;

    if (!parent || g_part_count >= MAX_PARTITIONS || count == 0u) {
        return;
    }

    idx = g_part_count++;
    c = &g_part_ctx[idx];
    d = &g_parts[idx];

    memset(c, 0, sizeof(*c));
    memset(d, 0, sizeof(*d));

    c->parent = parent;
    c->lba_start = start;
    c->lba_count = count;
    c->part_number_one_based = part_number_one_based;
    c->mbr_type = mbr_type;
    c->is_gpt = is_gpt;

    if (is_gpt) {
        if (gpt_type_guid) memcpy(c->gpt_type_guid, gpt_type_guid, 16u);
        if (gpt_part_guid) memcpy(c->gpt_part_guid, gpt_part_guid, 16u);
    }

    make_part_name(c, parent->name, part_number_one_based);

    d->name = c->name_buf;
    d->sector_size = parent->sector_size;
    d->total_sectors = count;
    d->present = parent->present;
    d->removable = parent->removable;
    d->ctx = c;
    d->read = part_read;
    d->write = part_write;
    d->flush = part_flush;
}

// ---------------- MBR parsing ----------------

typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} mbr_part_entry_t;

static uint32_t probe_mbr_partitions(block_device_t* parent, const uint8_t* lba0_512) {
    const uint8_t* mbr = lba0_512;
    const mbr_part_entry_t* pe = NULL;
    uint32_t added = 0;

    if (!parent || !mbr) {
        return 0;
    }

    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) {
        log_infof("block", "%s: no valid MBR signature; skipping MBR partitions",
                  parent->name ? parent->name : "(noname)");
        return 0;
    }

    pe = (const mbr_part_entry_t*)(mbr + 446);

    for (uint32_t i = 0; i < 4u && g_part_count < MAX_PARTITIONS; i++) {
        uint8_t type = pe[i].type;
        uint32_t start = pe[i].lba_start;
        uint32_t count = pe[i].lba_count;

        if (type == 0u || count == 0u) continue;

        if (type == 0xEEu) {
            log_infof("block", "%s: protective MBR detected; not registering as MBR partitions",
                      parent->name ? parent->name : "(noname)");
            continue;
        }

        register_partition(parent,
                           i + 1u,
                           (uint64_t)start,
                           (uint64_t)count,
                           false,
                           type,
                           NULL,
                           NULL);

        log_okf("block", "MBR partition %s: start=%x count=%x",
                g_parts[g_part_count - 1u].name ? g_parts[g_part_count - 1u].name : "(noname)",
                start,
                count);
        added++;
    }

    return added;
}

// ---------------- GPT parsing ----------------

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
    uint64_t part_entry_lba;
    uint32_t num_part_entries;
    uint32_t part_entry_size;
    uint32_t part_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name_utf16le[36];
} gpt_entry_t;

static bool probe_gpt_partitions(block_device_t* parent) {
    void* phys = NULL;
    uint8_t* hdrb = NULL;
    const gpt_header_t* gh = NULL;
    static const uint8_t sig[8] = {'E','F','I',' ','P','A','R','T'};

    if (!parent || !parent->read || parent->sector_size != 512u) return false;

    phys = pmm_alloc();
    if (!phys) {
        log_error("block", "GPT probe: pmm_alloc failed");
        return false;
    }
    hdrb = (uint8_t*)hhdm_phys_to_virt((uint64_t)(uintptr_t)phys);
    memset(hdrb, 0, 512u);

    if (!parent->read(parent, 1u, 1u, hdrb)) {
        log_errorf("block", "%s: GPT probe failed to read LBA1",
                   parent->name ? parent->name : "(noname)");
        pmm_free(phys);
        return false;
    }

    gh = (const gpt_header_t*)hdrb;
    for (int i = 0; i < 8; i++) {
        if (gh->signature[i] != sig[i]) {
            pmm_free(phys);
            return false;
        }
    }

    if (gh->header_size < 92u || gh->header_size > 512u) {
        log_errorf("block", "%s: GPT header_size looks wrong: %u",
                   parent->name ? parent->name : "(noname)",
                   gh->header_size);
        pmm_free(phys);
        return false;
    }
    if (gh->part_entry_size < sizeof(gpt_entry_t) || gh->part_entry_size > 1024u) {
        log_errorf("block", "%s: GPT part_entry_size unsupported: %u",
                   parent->name ? parent->name : "(noname)",
                   gh->part_entry_size);
        pmm_free(phys);
        return false;
    }
    if (gh->num_part_entries == 0u || gh->num_part_entries > 4096u) {
        log_errorf("block", "%s: GPT num_part_entries suspicious: %u",
                   parent->name ? parent->name : "(noname)",
                   gh->num_part_entries);
        pmm_free(phys);
        return false;
    }

    uint64_t entries_lba = gh->part_entry_lba;
    uint32_t entry_size = gh->part_entry_size;
    uint32_t nentries = gh->num_part_entries;
    uint64_t total_bytes = (uint64_t)entry_size * (uint64_t)nentries;
    uint32_t sectors_needed = (uint32_t)((total_bytes + 511u) / 512u);
    size_t pages = 0;
    void* phys2 = NULL;
    uint8_t* ent = NULL;
    uint32_t added = 0;

    pmm_free(phys);

    if (sectors_needed == 0u) return false;
    if (sectors_needed > 1024u) {
        log_errorf("block", "%s: GPT entries too large to read (%u sectors)",
                   parent->name ? parent->name : "(noname)",
                   sectors_needed);
        return false;
    }

    pages = (size_t)((sectors_needed * 512u + PAGE_SIZE - 1u) / PAGE_SIZE);
    phys2 = pmm_alloc_pages(pages);
    if (!phys2) {
        log_error("block", "GPT probe: pmm_alloc_pages failed for entries");
        return false;
    }

    ent = (uint8_t*)hhdm_phys_to_virt((uint64_t)(uintptr_t)phys2);
    memset(ent, 0, pages * PAGE_SIZE);

    if (!parent->read(parent, entries_lba, sectors_needed, ent)) {
        log_errorf("block", "%s: GPT probe failed to read partition entries",
                   parent->name ? parent->name : "(noname)");
        pmm_free_pages(phys2, pages);
        return false;
    }

    for (uint32_t i = 0; i < nentries && g_part_count < MAX_PARTITIONS; i++) {
        const uint8_t* ebase = ent + ((uint64_t)i * (uint64_t)entry_size);
        const gpt_entry_t* ge = (const gpt_entry_t*)ebase;
        uint64_t start = 0;
        uint64_t count = 0;

        if (guid_is_zero(ge->type_guid)) continue;
        if (ge->first_lba == 0u && ge->last_lba == 0u) continue;
        if (ge->last_lba < ge->first_lba) continue;

        start = ge->first_lba;
        count = (ge->last_lba - ge->first_lba) + 1u;

        register_partition(parent,
                           i + 1u,
                           start,
                           count,
                           true,
                           0u,
                           ge->type_guid,
                           ge->unique_guid);

        log_okf("block", "GPT partition %s: start=%x count=%x",
                g_parts[g_part_count - 1u].name ? g_parts[g_part_count - 1u].name : "(noname)",
                (uint32_t)start,
                (uint32_t)count);
        added++;
    }

    pmm_free_pages(phys2, pages);

    if (added == 0u) {
        log_infof("block", "%s: GPT present but no partitions found",
                  parent->name ? parent->name : "(noname)");
    } else {
        log_okf("block", "%s: GPT probe found %u partition(s)",
                parent->name ? parent->name : "(noname)",
                added);
    }

    return true;
}

static part_table_type_t probe_partitions_for_disk(block_device_t* disk) {
    void* phys0 = NULL;
    uint8_t* lba0 = NULL;
    bool has_gpt = false;
    uint32_t added_mbr = 0;

    if (!disk || !disk->read) {
        return PART_TABLE_NONE;
    }

    phys0 = pmm_alloc();
    if (!phys0) {
        log_error("block", "Partition probe: pmm_alloc failed for LBA0");
        return PART_TABLE_NONE;
    }

    lba0 = (uint8_t*)hhdm_phys_to_virt((uint64_t)(uintptr_t)phys0);
    memset(lba0, 0, 512u);

    if (!disk->read(disk, 0u, 1u, lba0)) {
        log_errorf("block", "%s: failed to read LBA0",
                   disk->name ? disk->name : "(noname)");
        pmm_free(phys0);
        return PART_TABLE_NONE;
    }

    has_gpt = probe_gpt_partitions(disk);
    if (has_gpt) {
        pmm_free(phys0);
        return PART_TABLE_GPT;
    }

    added_mbr = probe_mbr_partitions(disk, lba0);
    pmm_free(phys0);

    if (added_mbr > 0u) {
        return PART_TABLE_MBR;
    }

    return PART_TABLE_NONE;
}

// ---------------- public API ----------------

void block_init(void) {
    uint32_t ahci_count = 0;
    uint32_t usb_count = 0;

    g_disk_count = 0u;
    g_part_count = 0u;
    g_boot_part_table = PART_TABLE_NONE;
    g_ready = false;
    g_registered_ahci_count = 0u;
    g_registered_usb_count = 0u;
    memset(g_disks, 0, sizeof(g_disks));
    memset(g_disk_ctx, 0, sizeof(g_disk_ctx));
    memset(g_parts, 0, sizeof(g_parts));
    memset(g_part_ctx, 0, sizeof(g_part_ctx));

    (void)usb_storage_rescan();

    ahci_count = ahci_disk_count();
    usb_count = usb_storage_disk_count();

    for (uint32_t i = 0; i < ahci_count && g_disk_count < MAX_DISKS; i++) {
        block_device_t* dev = register_disk(DISK_BACKEND_AHCI, i);
        if (!dev) {
            break;
        }

        log_okf("block", "Registered disk %s (sector=%u)",
                dev->name ? dev->name : "(noname)",
                dev->sector_size);
        g_registered_ahci_count++;
    }

    for (uint32_t i = 0; i < usb_count && g_disk_count < MAX_DISKS; i++) {
        block_device_t* dev = register_disk(DISK_BACKEND_USB_STORAGE, i);
        if (!dev) {
            break;
        }

        log_okf("block", "Registered USB disk %s (sector=%u)",
                dev->name ? dev->name : "(noname)",
                dev->sector_size);
        g_registered_usb_count++;
    }

    if (g_disk_count == 0u) {
        log_error("block", "No block devices registered");
        return;
    }

    g_ready = true;
    log_okf("block", "Boot block device: %s (sector=%u)",
            g_disks[0].name ? g_disks[0].name : "(noname)",
            g_disks[0].sector_size);

    for (uint32_t i = 0; i < g_disk_count; i++) {
        disk_ctx_t* ctx = (disk_ctx_t*)g_disks[i].ctx;
        if (!ctx) {
            continue;
        }
        sync_disk_presence(&g_disks[i]);
        if (!g_disks[i].present) {
            continue;
        }
        ctx->part_table = probe_partitions_for_disk(&g_disks[i]);
        if (i == 0u) {
            g_boot_part_table = ctx->part_table;
        }
    }
}

uint32_t block_rescan(void) {
    uint32_t ahci_count = 0;
    uint32_t usb_count = 0;
    uint32_t added = 0;

    sync_all_device_presence();
    pci_enumerate_and_log();
    (void)usb_storage_full_rescan();
    sync_all_device_presence();

    ahci_count = ahci_disk_count();
    usb_count = usb_storage_disk_count();

    for (uint32_t i = g_registered_ahci_count; i < ahci_count && g_disk_count < MAX_DISKS; i++) {
        block_device_t* dev = register_disk(DISK_BACKEND_AHCI, i);
        disk_ctx_t* ctx = NULL;

        if (!dev) {
            break;
        }

        log_okf("block", "Registered disk %s (sector=%u)",
                dev->name ? dev->name : "(noname)",
                dev->sector_size);

        ctx = (disk_ctx_t*)dev->ctx;
        if (ctx && dev->present) {
            ctx->part_table = probe_partitions_for_disk(dev);
            if (i == 0u) {
                g_boot_part_table = ctx->part_table;
            }
        }

        added++;
        g_registered_ahci_count++;
    }

    for (uint32_t i = g_registered_usb_count; i < usb_count && g_disk_count < MAX_DISKS; i++) {
        block_device_t* dev = register_disk(DISK_BACKEND_USB_STORAGE, i);
        disk_ctx_t* ctx = NULL;

        if (!dev) {
            break;
        }

        log_okf("block", "Registered USB disk %s (sector=%u)",
                dev->name ? dev->name : "(noname)",
                dev->sector_size);

        ctx = (disk_ctx_t*)dev->ctx;
        if (ctx && dev->present) {
            ctx->part_table = probe_partitions_for_disk(dev);
        }

        added++;
        g_registered_usb_count++;
    }

    if (g_disk_count > 0u) {
        g_ready = true;
    }

    return added;
}

uint32_t block_poll_hotplug(void) {
    uint32_t usb_count = 0;
    uint32_t added = 0;

    // Poll only hotplug-capable transports here. Full PCI rescans stay in
    // block_rescan(), because this can run from scheduler idle paths.
    sync_all_device_presence();
    (void)usb_storage_rescan();
    sync_all_device_presence();
    usb_count = usb_storage_disk_count();

    for (uint32_t i = g_registered_usb_count; i < usb_count && g_disk_count < MAX_DISKS; i++) {
        block_device_t* dev = register_disk(DISK_BACKEND_USB_STORAGE, i);
        disk_ctx_t* ctx = NULL;

        if (!dev) {
            break;
        }

        log_okf("block", "Registered USB disk %s (sector=%u)",
                dev->name ? dev->name : "(noname)",
                dev->sector_size);

        ctx = (disk_ctx_t*)dev->ctx;
        if (ctx && dev->present) {
            ctx->part_table = probe_partitions_for_disk(dev);
        }

        added++;
        g_registered_usb_count++;
    }

    if (g_disk_count > 0u) {
        g_ready = true;
    }

    return added;
}

block_device_t* block_boot_device(void) {
    if (!g_ready || g_disk_count == 0u) return 0;
    sync_all_device_presence();
    return &g_disks[0];
}

bool block_device_is_present(block_device_t* dev) {
    if (!dev) {
        return false;
    }

    if (dev->ctx) {
        for (uint32_t i = 0; i < g_part_count; i++) {
            if (&g_parts[i] == dev) {
                sync_part_presence(dev);
                return dev->present;
            }
        }

        sync_disk_presence(dev);
    }

    return dev->present;
}

bool block_device_is_removable(const block_device_t* dev) {
    return dev && dev->removable;
}

uint32_t block_disk_count(void) {
    sync_all_device_presence();
    return g_disk_count;
}

block_device_t* block_disk_device(uint32_t index) {
    if (index >= g_disk_count) return 0;
    sync_disk_presence(&g_disks[index]);
    return &g_disks[index];
}

block_device_t* block_device_by_name(const char* name) {
    if (!name || !*name) {
        return NULL;
    }

    sync_all_device_presence();

    for (uint32_t i = 0; i < g_disk_count; i++) {
        if (g_disks[i].present && g_disks[i].name && strcmp(g_disks[i].name, name) == 0) {
            return &g_disks[i];
        }
    }

    for (uint32_t i = 0; i < g_part_count; i++) {
        if (g_parts[i].present && g_parts[i].name && strcmp(g_parts[i].name, name) == 0) {
            return &g_parts[i];
        }
    }

    return NULL;
}

uint32_t block_partition_count(void) {
    sync_all_device_presence();
    return g_part_count;
}

block_device_t* block_partition_device(uint32_t index) {
    if (index >= g_part_count) return 0;
    sync_part_presence(&g_parts[index]);
    return &g_parts[index];
}

block_device_t* block_partition_lookup(uint32_t disk_index, uint32_t part_number_one_based) {
    block_device_t* disk = NULL;

    if (part_number_one_based == 0u) {
        return NULL;
    }

    disk = block_disk_device(disk_index);
    if (!disk || !disk->present) {
        return NULL;
    }

    for (uint32_t i = 0; i < g_part_count; i++) {
        if (g_part_ctx[i].parent == disk &&
            g_part_ctx[i].part_number_one_based == part_number_one_based &&
            g_parts[i].present) {
            return &g_parts[i];
        }
    }

    return NULL;
}

part_table_type_t block_partition_table_type(void) {
    return g_boot_part_table;
}

part_table_type_t block_disk_partition_table_type(uint32_t disk_index) {
    if (disk_index >= g_disk_count) {
        return PART_TABLE_NONE;
    }
    sync_disk_presence(&g_disks[disk_index]);
    if (!g_disks[disk_index].present) {
        return PART_TABLE_NONE;
    }
    return g_disk_ctx[disk_index].part_table;
}
