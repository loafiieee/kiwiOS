#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "drivers/ahci/ahci.h"
#include "core/log.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "libc/string.h" // memset, memcpy

// HBA global regs offsets
#define AHCI_HBA_CAP  0x00
#define AHCI_HBA_GHC  0x04
#define AHCI_HBA_IS   0x08
#define AHCI_HBA_PI   0x0C
#define AHCI_HBA_VS   0x10

// Per-port base: 0x100 + port*0x80
#define AHCI_PORT_BASE      0x100
#define AHCI_PORT_STRIDE    0x80

// Port regs
#define PxCLB   0x00
#define PxCLBU  0x04
#define PxFB    0x08
#define PxFBU   0x0C
#define PxIS    0x10
#define PxIE    0x14
#define PxCMD   0x18
#define PxTFD   0x20
#define PxSIG   0x24
#define PxSSTS  0x28
#define PxSERR  0x30
#define PxCI    0x38

// PxCMD bits
#define PxCMD_ST   (1u << 0)
#define PxCMD_FRE  (1u << 4)
#define PxCMD_FR   (1u << 14)
#define PxCMD_CR   (1u << 15)

// FIS types
#define FIS_TYPE_REG_H2D 0x27

// ATA commands (48-bit LBA DMA EXT)
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

// SATA device signature values
#define SATA_SIG_ATA   0x00000101
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_SEMB  0xC33C0101
#define SATA_SIG_PM    0x96690101

// ----- MMIO mapping scheme -----
// Map multiple AHCI controllers without VA collisions.
#define AHCI_MMIO_VIRT_BASE 0xFFFFFFFFA0000000ULL
#define AHCI_MMIO_SLOTS 16

static uint64_t g_mmio_phys_pages[AHCI_MMIO_SLOTS] = {0};

static volatile uint8_t* ahci_map(uint64_t mmio_phys) {
    page_table_t* kpt = vmm_get_kernel_page_table();

    uint64_t phys_page = PAGE_ALIGN_DOWN(mmio_phys);
    uint64_t off = mmio_phys - phys_page;

    // Already mapped?
    for (int i = 0; i < AHCI_MMIO_SLOTS; i++) {
        if (g_mmio_phys_pages[i] == phys_page) {
            return (volatile uint8_t*)(AHCI_MMIO_VIRT_BASE + (uint64_t)i * PAGE_SIZE + off);
        }
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < AHCI_MMIO_SLOTS; i++) {
        if (g_mmio_phys_pages[i] == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        log_error("ahci", "MMIO map slots exhausted (increase AHCI_MMIO_SLOTS)");
        return 0;
    }

    uint64_t virt_page = AHCI_MMIO_VIRT_BASE + (uint64_t)slot * PAGE_SIZE;
    if (!vmm_map_page(kpt, virt_page, phys_page, PAGE_PRESENT | PAGE_WRITE)) {
        log_error("ahci", "Failed to map AHCI MMIO page");
        return 0;
    }

    g_mmio_phys_pages[slot] = phys_page;
    return (volatile uint8_t*)(virt_page + off);
}

static inline uint32_t rd32(volatile uint8_t* b, uint32_t off) {
    return *(volatile uint32_t*)(b + off);
}
static inline void wr32(volatile uint8_t* b, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(b + off) = v;
}

static const char* sig_name(uint32_t sig) {
    if (sig == SATA_SIG_ATA)   return "SATA";
    if (sig == SATA_SIG_ATAPI) return "ATAPI";
    if (sig == SATA_SIG_SEMB)  return "SEMB";
    if (sig == SATA_SIG_PM)    return "PM";
    return "Unknown";
}

// -------------------- AHCI DMA structures --------------------

// Command header (32 entries in command list)
typedef struct __attribute__((packed)) {
    // DW0
    uint8_t  cfl:5;     // command FIS length in DWORDS
    uint8_t  a:1;       // ATAPI
    uint8_t  w:1;       // write
    uint8_t  p:1;       // prefetchable

    uint8_t  r:1;       // reset
    uint8_t  b:1;       // bist
    uint8_t  c:1;       // clear busy upon R_OK
    uint8_t  rsv0:1;
    uint8_t  pmp:4;     // port multiplier

    uint16_t prdtl;     // PRDT length (entries)

    // DW1
    volatile uint32_t prdbc; // bytes transferred

    // DW2-3
    uint32_t ctba;      // command table base addr
    uint32_t ctbau;

    // DW4-7
    uint32_t rsv1[4];
} hba_cmd_header_t;

typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} hba_prdt_t;

// Register Host-to-Device FIS
typedef struct __attribute__((packed)) {
    uint8_t fis_type;   // 0x27
    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;        // 1=command
    uint8_t command;
    uint8_t featurel;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;

    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;

    uint8_t rsv1[4];
} fis_reg_h2d_t;

/*
  Command table is exactly 4 KiB (one page) in our current design.
  Layout is 128 bytes of fixed area + PRDT entries.

  Fixed area:
    cfis[64] + acmd[16] + rsv[48] = 128 bytes

  Remaining for PRDT:
    4096 - 128 = 3968 bytes
    Each PRDT entry is 16 bytes => 3968 / 16 = 248 entries max.
*/
#define AHCI_MAX_PRDT 248

typedef struct __attribute__((packed)) {
    uint8_t cfis[64];   // command FIS
    uint8_t acmd[16];   // ATAPI command (unused)
    uint8_t rsv[48];
    hba_prdt_t prdt[AHCI_MAX_PRDT];
} hba_cmd_table_t;

// -------------------- helpers --------------------

static void ata_swap_model(char* out, const uint16_t* id_words, uint32_t word_start, uint32_t word_count) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < word_count; i++) {
        uint16_t w = id_words[word_start + i];
        out[j++] = (char)(w >> 8);
        out[j++] = (char)(w & 0xFF);
    }
    out[j] = 0;
    for (int k = (int)j - 1; k >= 0; k--) {
        if (out[k] == ' ' || out[k] == '\0') out[k] = 0;
        else break;
    }
}

static void port_stop(volatile uint8_t* base, uint32_t port) {
    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    // Clear ST
    uint32_t cmd = rd32(base, px + PxCMD);
    cmd &= ~PxCMD_ST;
    wr32(base, px + PxCMD, cmd);

    // Wait for CR clear
    for (int i = 0; i < 20000; i++) {
        if ((rd32(base, px + PxCMD) & PxCMD_CR) == 0) break;
        asm volatile("pause");
    }

    // Clear FRE
    cmd = rd32(base, px + PxCMD);
    cmd &= ~PxCMD_FRE;
    wr32(base, px + PxCMD, cmd);

    // Wait for FR clear
    for (int i = 0; i < 20000; i++) {
        if ((rd32(base, px + PxCMD) & PxCMD_FR) == 0) break;
        asm volatile("pause");
    }
}

static void port_start(volatile uint8_t* base, uint32_t port) {
    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    uint32_t cmd = rd32(base, px + PxCMD);

    // Set FRE then ST
    cmd |= PxCMD_FRE;
    wr32(base, px + PxCMD, cmd);

    cmd |= PxCMD_ST;
    wr32(base, px + PxCMD, cmd);
}

static bool port_wait_not_busy(volatile uint8_t* base, uint32_t port) {
    // PxTFD bits: BSY=0x80, DRQ=0x08
    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;
    for (int i = 0; i < 200000; i++) {
        uint32_t tfd = rd32(base, px + PxTFD);
        if ((tfd & (0x80u | 0x08u)) == 0) return true; // not BSY, not DRQ
        asm volatile("pause");
    }
    return false;
}

static bool port_issue_and_wait(volatile uint8_t* base, uint32_t port, uint32_t slot_mask) {
    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    // Issue
    wr32(base, px + PxCI, slot_mask);

    // Wait for completion (CI bit clears)
    for (int i = 0; i < 400000; i++) {
        if ((rd32(base, px + PxCI) & slot_mask) == 0) break;
        asm volatile("pause");
    }

    uint32_t ci = rd32(base, px + PxCI);
    if (ci & slot_mask) {
        uint32_t tfd  = rd32(base, px + PxTFD);
        uint32_t is   = rd32(base, px + PxIS);
        uint32_t serr = rd32(base, px + PxSERR);
        log_errorf("ahci", "CMD timeout: CI=%x TFD=%x IS=%x SERR=%x", ci, tfd, is, serr);
        return false;
    }

    // Check error
    uint32_t tfd = rd32(base, px + PxTFD);
    if (tfd & 0x01u) {
        uint32_t is   = rd32(base, px + PxIS);
        uint32_t serr = rd32(base, px + PxSERR);
        log_errorf("ahci", "CMD error: TFD=%x IS=%x SERR=%x", tfd, is, serr);
        return false;
    }

    return true;
}

/*
  VA->PA translation for DMA:
    - If VA is in HHDM: PA = VA - hhdm_offset
    - Else use page table walk for the page base + preserve offset
*/
static bool virt_to_phys_any(uint64_t va, uint64_t* out_pa) {
    uint64_t off = hhdm_get_offset();

    if (va >= off) {
        uint64_t pa = va - off;
        if (pa < (1ULL << 46)) {
            *out_pa = pa;
            return true;
        }
    }

    page_table_t* kpt = vmm_get_kernel_page_table();
    uint64_t page_va = PAGE_ALIGN_DOWN(va);
    uint64_t page_pa = vmm_get_physical(kpt, page_va);
    if (page_pa == 0) return false;

    *out_pa = page_pa + (va & 0xFFFULL);
    return true;
}

// -------------------- global selected disk state --------------------

typedef struct {
    bool     ready;
    uint32_t mmio_phys32;
    uint32_t port;

    uint64_t clb_phys;
    uint64_t fb_phys;
    uint64_t ct_phys;

    volatile uint8_t* mmio_base;
} ahci_disk_t;

static ahci_disk_t g_disk = {0};

// -------------------- probe helpers --------------------

static bool ahci_init_port(uint32_t mmio_phys32, uint32_t port) {
    volatile uint8_t* base = ahci_map((uint64_t)mmio_phys32);
    if (!base) return false;

    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    // Must be active
    uint32_t ssts = rd32(base, px + PxSSTS);
    uint32_t det = ssts & 0x0F;
    uint32_t ipm = (ssts >> 8) & 0x0F;
    if (!(det == 3 && ipm == 1)) {
        log_errorf("ahci", "Init port %u failed: not active (SSTS=%x)", port, ssts);
        return false;
    }

    uint32_t sig = rd32(base, px + PxSIG);
    if (sig != SATA_SIG_ATA) {
        log_errorf("ahci", "Init port %u failed: not SATA (SIG=%x %s)", port, sig, sig_name(sig));
        return false;
    }

    // Stop port, clear errors/interrupts
    port_stop(base, port);
    wr32(base, px + PxSERR, 0xFFFFFFFFu);
    wr32(base, px + PxIS,   0xFFFFFFFFu);

    // Allocate DMA structures (one page each)
    void* clb_page = pmm_alloc();
    void* fb_page  = pmm_alloc();
    void* ct_page  = pmm_alloc();

    uint64_t clb_phys = (uint64_t)(uintptr_t)clb_page;
    uint64_t fb_phys  = (uint64_t)(uintptr_t)fb_page;
    uint64_t ct_phys  = (uint64_t)(uintptr_t)ct_page;

    if (!clb_phys || !fb_phys || !ct_phys) {
        log_error("ahci", "Failed to allocate DMA pages for CLB/FB/CT");
        return false;
    }

    void* clb_virt = phys_to_virt(clb_phys);
    void* fb_virt  = phys_to_virt(fb_phys);
    void* ct_virt  = phys_to_virt(ct_phys);

    memset(clb_virt, 0, PAGE_SIZE);
    memset(fb_virt,  0, PAGE_SIZE);
    memset(ct_virt,  0, PAGE_SIZE);

    // Program CLB/FB
    wr32(base, px + PxCLB,  (uint32_t)clb_phys);
    wr32(base, px + PxCLBU, (uint32_t)(clb_phys >> 32));
    wr32(base, px + PxFB,   (uint32_t)fb_phys);
    wr32(base, px + PxFBU,  (uint32_t)(fb_phys >> 32));

    // Start command engine
    port_start(base, port);

    g_disk.ready = true;
    g_disk.mmio_phys32 = mmio_phys32;
    g_disk.port = port;
    g_disk.clb_phys = clb_phys;
    g_disk.fb_phys  = fb_phys;
    g_disk.ct_phys  = ct_phys;
    g_disk.mmio_base = base;

    log_okf("ahci", "Port %u initialized: CLB=%x FB=%x CT=%x",
            port, (uint32_t)clb_phys, (uint32_t)fb_phys, (uint32_t)ct_phys);

    return true;
}

static bool ahci_identify_selected_disk(void) {
    if (!g_disk.ready) return false;

    volatile uint8_t* base = g_disk.mmio_base;
    uint32_t port = g_disk.port;
    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    if (!port_wait_not_busy(base, port)) {
        log_error("ahci", "IDENTIFY: port stayed busy");
        return false;
    }

    wr32(base, px + PxSERR, 0xFFFFFFFFu);
    wr32(base, px + PxIS,   0xFFFFFFFFu);

    // Identify buffer: one page, use first 512 bytes
    void* id_page = pmm_alloc();
    uint64_t id_phys = (uint64_t)(uintptr_t)id_page;
    void* id_virt = phys_to_virt(id_phys);
    memset(id_virt, 0, PAGE_SIZE);

    hba_cmd_header_t* cmd_list = (hba_cmd_header_t*)phys_to_virt(g_disk.clb_phys);
    hba_cmd_table_t*  ct       = (hba_cmd_table_t*)phys_to_virt(g_disk.ct_phys);

    memset(cmd_list, 0, 32 * sizeof(hba_cmd_header_t));
    memset(ct, 0, PAGE_SIZE);

    hba_cmd_header_t* ch = &cmd_list[0];
    ch->cfl = sizeof(fis_reg_h2d_t) / 4;
    ch->w = 0;
    ch->prdtl = 1;
    ch->prdbc = 0;
    ch->ctba  = (uint32_t)g_disk.ct_phys;
    ch->ctbau = (uint32_t)(g_disk.ct_phys >> 32);

    ct->prdt[0].dba  = (uint32_t)id_phys;
    ct->prdt[0].dbau = (uint32_t)(id_phys >> 32);
    ct->prdt[0].dbc  = 512 - 1;
    ct->prdt[0].i    = 1;

    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)(&ct->cfis[0]);
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_IDENTIFY;
    fis->device = 0;

    if (!port_issue_and_wait(base, port, 1u << 0)) {
        log_error("ahci", "IDENTIFY failed");
        return false;
    }

    const uint16_t* idw = (const uint16_t*)id_virt;
    char model[41];
    ata_swap_model(model, idw, 27, 20);
    log_okf("ahci", "IDENTIFY OK: model='%s'", model);
    return true;
}

// -------------------- public API --------------------

bool ahci_disk_ready(void) {
    return g_disk.ready;
}

static bool ahci_rw(uint8_t ata_cmd, uint64_t lba, uint32_t sector_count, void* buffer) {
    if (!g_disk.ready) {
        log_error("ahci", "rw: no disk selected");
        return false;
    }

    if (sector_count == 0) return false;

    // ATA READ/WRITE DMA EXT uses a 16-bit sector count field.
    // value 0 means 65536 sectors; we won't support >65536 in one command.
    if (sector_count > 65536u) {
        log_errorf("ahci", "rw: sector_count too large (%u), max 65536", sector_count);
        return false;
    }

    uint64_t bytes = (uint64_t)sector_count * 512ull;
    if (bytes == 0) return false;

    // PRDT count check (worst case: 1 PRDT per page)
    // Worst-case pages = ceil((start_off + bytes)/PAGE_SIZE).
    uint64_t va0 = (uint64_t)(uintptr_t)buffer;
    uint64_t start_off = va0 & 0xFFFULL;
    uint64_t pages_needed = (start_off + bytes + (PAGE_SIZE - 1)) / PAGE_SIZE;
    if (pages_needed > AHCI_MAX_PRDT) {
        log_errorf("ahci", "rw: request too large for 4KiB command table PRDT (%u pages > %u)",
                   (uint32_t)pages_needed, (uint32_t)AHCI_MAX_PRDT);
        return false;
    }

    volatile uint8_t* base = g_disk.mmio_base;
    uint32_t port = g_disk.port;
    uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    if (!port_wait_not_busy(base, port)) {
        log_error("ahci", "rw: port stayed busy");
        return false;
    }

    wr32(base, px + PxSERR, 0xFFFFFFFFu);
    wr32(base, px + PxIS,   0xFFFFFFFFu);

    // Command list + table (slot 0)
    hba_cmd_header_t* cmd_list = (hba_cmd_header_t*)phys_to_virt(g_disk.clb_phys);
    hba_cmd_table_t*  ct       = (hba_cmd_table_t*)phys_to_virt(g_disk.ct_phys);

    memset(cmd_list, 0, 32 * sizeof(hba_cmd_header_t));
    memset(ct, 0, PAGE_SIZE);

    hba_cmd_header_t* ch = &cmd_list[0];
    ch->cfl = sizeof(fis_reg_h2d_t) / 4;
    ch->w = (ata_cmd == ATA_CMD_WRITE_DMA_EXT) ? 1 : 0;
    ch->prdbc = 0;
    ch->ctba  = (uint32_t)g_disk.ct_phys;
    ch->ctbau = (uint32_t)(g_disk.ct_phys >> 32);

    // Build PRDT (split across pages, preserve offsets)
    uint64_t remaining = bytes;
    uint64_t va = (uint64_t)(uintptr_t)buffer;
    uint32_t prdt_count = 0;

    while (remaining > 0) {
        if (prdt_count >= AHCI_MAX_PRDT) {
            log_error("ahci", "rw: PRDT overflow (internal bug)");
            return false;
        }

        uint64_t pa = 0;
        if (!virt_to_phys_any(va, &pa)) {
            log_errorf("ahci", "rw: could not translate buffer VA->PA (va=%x)", (uint32_t)va);
            return false;
        }

        uint64_t page_off = va & 0xFFFULL;
        uint64_t chunk = (uint64_t)PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;

        ct->prdt[prdt_count].dba  = (uint32_t)pa;
        ct->prdt[prdt_count].dbau = (uint32_t)(pa >> 32);
        ct->prdt[prdt_count].dbc  = (uint32_t)(chunk - 1);
        ct->prdt[prdt_count].i    = 0;

        va += chunk;
        remaining -= chunk;
        prdt_count++;
    }

    // Interrupt on completion for the last PRDT entry
    if (prdt_count > 0) {
        ct->prdt[prdt_count - 1].i = 1;
    }

    ch->prdtl = (uint16_t)prdt_count;

    // Fill FIS
    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)(&ct->cfis[0]);
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ata_cmd;

    // 48-bit LBA
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->device = 1u << 6; // LBA mode

    // sector count (16-bit). 0 means 65536 sectors.
    if (sector_count == 65536u) {
        fis->countl = 0;
        fis->counth = 0;
    } else {
        fis->countl = (uint8_t)(sector_count & 0xFF);
        fis->counth = (uint8_t)((sector_count >> 8) & 0xFF);
    }

    if (!port_issue_and_wait(base, port, 1u << 0)) {
        uint32_t lba_lo = (uint32_t)(lba & 0xFFFFFFFFu);
        uint32_t lba_hi = (uint32_t)((lba >> 32) & 0xFFFFFFFFu);
        log_errorf("ahci", "rw failed cmd=%x lba_hi=%x lba_lo=%x count=%u prdt=%u",
                   (unsigned)ata_cmd, lba_hi, lba_lo, sector_count, prdt_count);
        return false;
    }

    // Optional sanity (comment out if noisy):
    // if (ch->prdbc != (uint32_t)bytes) {
    //     log_infof("ahci", "rw: prdbc=%x expected=%x", ch->prdbc, (uint32_t)bytes);
    // }

    return true;
}

bool ahci_read(uint64_t lba, uint32_t sector_count, void* buffer) {
    return ahci_rw(ATA_CMD_READ_DMA_EXT, lba, sector_count, buffer);
}

bool ahci_write(uint64_t lba, uint32_t sector_count, const void* buffer) {
    return ahci_rw(ATA_CMD_WRITE_DMA_EXT, lba, sector_count, (void*)buffer);
}

bool ahci_flush(void) {
    // You can implement real CACHE FLUSH EXT (0xEA) later.
    // For now, ordering is mainly tested at higher layers.
    return true;
}

// -------------------- probe --------------------

void ahci_probe_mmio(uint32_t mmio_phys32) {
    volatile uint8_t* base = ahci_map((uint64_t)mmio_phys32);
    if (!base) return;

    uint32_t cap = rd32(base, AHCI_HBA_CAP);
    uint32_t ghc = rd32(base, AHCI_HBA_GHC);
    uint32_t pi  = rd32(base, AHCI_HBA_PI);
    uint32_t vs  = rd32(base, AHCI_HBA_VS);

    log_infof("ahci", "HBA mmio=%x CAP=%x GHC=%x PI=%x VS=%x",
              mmio_phys32, cap, ghc, pi, vs);

    // CAP lower 5 bits = # ports - 1
    uint32_t ports_reported = (cap & 0x1Fu) + 1u;
    log_infof("ahci", "CAP reports %u ports; PI bitmask=%x", ports_reported, pi);

    for (uint32_t port = 0; port < ports_reported; port++) {
        if ((pi & (1u << port)) == 0) continue;

        uint32_t px = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;
        uint32_t ssts = rd32(base, px + PxSSTS);
        uint32_t det = ssts & 0x0F;
        uint32_t ipm = (ssts >> 8) & 0x0F;
        uint32_t sig = rd32(base, px + PxSIG);

        bool active = (det == 3 && ipm == 1);
        log_infof("ahci", "Port %u: SSTS=%x DET=%u IPM=%u SIG=%x (%s) %s",
                  port, ssts, det, ipm, sig, sig_name(sig),
                  active ? "[ACTIVE]" : "[EMPTY]");

        if (!active) continue;
        if (sig != SATA_SIG_ATA) continue;

        if (!g_disk.ready) {
            log_infof("ahci", "Selecting port %u for disk I/O", port);
            if (ahci_init_port(mmio_phys32, port)) {
                (void)ahci_identify_selected_disk();
            }
        }
    }
}
