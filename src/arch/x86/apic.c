#include "arch/x86/apic.h"

#include <stddef.h>
#include <stdint.h>

#include "core/console.h"
#include "core/log.h"
#include "drivers/acpi/acpi.h"
#include "libc/string.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"

#define LAPIC_MMIO_VIRT 0xFFFFFFFFA4000000ull
#define IOAPIC_MMIO_VIRT_BASE 0xFFFFFFFFA4100000ull
#define IOAPIC_MMIO_PAGES APIC_MAX_IOAPICS

#define LAPIC_REG_ID 0x020u
#define LAPIC_REG_VERSION 0x030u
#define LAPIC_REG_EOI 0x0B0u
#define LAPIC_REG_SVR 0x0F0u

#define IOAPIC_REG_ID 0x00u
#define IOAPIC_REG_VERSION 0x01u
#define IOAPIC_REG_REDIR_BASE 0x10u

#define IOAPIC_REDIR_MASKED (1u << 16)

static volatile uint8_t* g_lapic_mmio = NULL;
static volatile uint8_t* g_ioapic_mmio[APIC_MAX_IOAPICS];
static lapic_probe_info_t g_lapic;
static ioapic_probe_info_t g_ioapics[APIC_MAX_IOAPICS];
static uint32_t g_ioapic_count = 0;
static bool g_apic_probe_available = false;

static uint32_t ioapic_read_with_echo(uint32_t index, uint32_t reg, uint32_t* out_selector_echo);

static bool map_mmio_page(uint64_t virt, uint64_t phys) {
    page_table_t* kpt = vmm_get_kernel_page_table();

    if (!kpt || phys == 0) {
        return false;
    }

    return vmm_map_page(kpt,
                        PAGE_ALIGN_DOWN(virt),
                        PAGE_ALIGN_DOWN(phys),
                        PAGE_WRITE | PAGE_WRITETHROUGH | PAGE_CACHE_DISABLE);
}

static uint32_t lapic_read(uint32_t reg) {
    if (!g_lapic_mmio) {
        return 0;
    }

    return *(volatile uint32_t*)(g_lapic_mmio + reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    if (!g_lapic_mmio) {
        return;
    }

    *(volatile uint32_t*)(g_lapic_mmio + reg) = value;
}

static uint32_t ioapic_read(uint32_t index, uint32_t reg) {
    return ioapic_read_with_echo(index, reg, NULL);
}

static uint32_t ioapic_read_with_echo(uint32_t index, uint32_t reg, uint32_t* out_selector_echo) {
    volatile uint32_t* select = NULL;
    volatile uint32_t* window = NULL;

    if (index >= g_ioapic_count || !g_ioapic_mmio[index]) {
        if (out_selector_echo) {
            *out_selector_echo = 0;
        }
        return 0;
    }

    select = (volatile uint32_t*)(g_ioapic_mmio[index] + 0x00u);
    window = (volatile uint32_t*)(g_ioapic_mmio[index] + 0x10u);

    *select = reg;
    if (out_selector_echo) {
        *out_selector_echo = *select;
    }
    return *window;
}

static void ioapic_write(uint32_t index, uint32_t reg, uint32_t value) {
    volatile uint32_t* select = NULL;
    volatile uint32_t* window = NULL;

    if (index >= g_ioapic_count || !g_ioapic_mmio[index]) {
        return;
    }

    select = (volatile uint32_t*)(g_ioapic_mmio[index] + 0x00u);
    window = (volatile uint32_t*)(g_ioapic_mmio[index] + 0x10u);

    *select = reg;
    *window = value;
}

static void sample_redir_entry(uint32_t ioapic_index, uint32_t redir_index) {
    ioapic_probe_info_t* ioapic = NULL;
    ioapic_redir_probe_info_t* out = NULL;
    uint32_t low = 0;
    uint32_t high = 0;

    if (ioapic_index >= g_ioapic_count || redir_index >= APIC_MAX_IOAPIC_REDIRS) {
        return;
    }

    ioapic = &g_ioapics[ioapic_index];
    out = &ioapic->redirs[redir_index];
    low = ioapic_read(ioapic_index, IOAPIC_REG_REDIR_BASE + redir_index * 2u);
    high = ioapic_read(ioapic_index, IOAPIC_REG_REDIR_BASE + redir_index * 2u + 1u);

    out->present = true;
    out->gsi = ioapic->gsi_base + redir_index;
    out->low = low;
    out->high = high;
    out->vector = (uint8_t)(low & 0xFFu);
    out->delivery_mode = (uint8_t)((low >> 8) & 0x7u);
    out->dest = (uint8_t)((high >> 24) & 0xFFu);
    out->masked = (low & IOAPIC_REDIR_MASKED) != 0u;
    out->remote_irr = (low & (1u << 14)) != 0u;
    out->polarity_low = (low & (1u << 13)) != 0u;
    out->trigger_level = (low & (1u << 15)) != 0u;
}

void apic_probe_from_acpi(void) {
    const acpi_madt_info_t* madt = acpi_madt_info();

    memset(&g_lapic, 0, sizeof(g_lapic));
    memset(g_ioapics, 0, sizeof(g_ioapics));
    memset((void*)g_ioapic_mmio, 0, sizeof(g_ioapic_mmio));
    g_lapic_mmio = NULL;
    g_ioapic_count = 0;
    g_apic_probe_available = false;

    if (!acpi_available() || !madt || !madt->present) {
        log_error("apic", "MADT is unavailable; APIC probe skipped");
        return;
    }

    if (madt->lapic_address != 0 &&
        map_mmio_page(LAPIC_MMIO_VIRT, madt->lapic_address)) {
        uint32_t version = 0;

        g_lapic_mmio = (volatile uint8_t*)LAPIC_MMIO_VIRT;
        version = lapic_read(LAPIC_REG_VERSION);

        g_lapic.present = true;
        g_lapic.phys = madt->lapic_address;
        g_lapic.id = lapic_read(LAPIC_REG_ID) >> 24;
        g_lapic.version = version & 0xFFu;
        g_lapic.max_lvt = (version >> 16) & 0xFFu;
        g_lapic.svr = lapic_read(LAPIC_REG_SVR);

        log_okf("apic",
                "LAPIC mapped phys_hi=%x phys_lo=%x id=%u version=%x max_lvt=%u",
                (uint32_t)(g_lapic.phys >> 32),
                (uint32_t)g_lapic.phys,
                g_lapic.id,
                g_lapic.version,
                g_lapic.max_lvt);
    } else {
        log_error("apic", "failed to map LAPIC");
    }

    for (uint32_t i = 0; i < madt->ioapic_count && i < APIC_MAX_IOAPICS; i++) {
        uint64_t virt = IOAPIC_MMIO_VIRT_BASE + ((uint64_t)i * PAGE_SIZE);
        uint32_t raw_id = 0;
        uint32_t raw_ver = 0;
        uint32_t id_echo = 0;
        uint32_t ver_echo = 0;
        uint32_t redir_count = 0;
        uint32_t redirs_sampled = 0;
        uint64_t mapped_phys = 0;
        uint64_t mapped_flags = 0;
        uint64_t hhdm_virt = 0;
        uint64_t hhdm_phys = 0;
        uint64_t hhdm_flags = 0;
        uint32_t slot = g_ioapic_count;

        if (!map_mmio_page(virt, madt->ioapics[i].address)) {
            log_errorf("apic", "failed to map IOAPIC %u", i);
            continue;
        }

        g_ioapic_mmio[slot] = (volatile uint8_t*)virt;
        g_ioapic_count = slot + 1u;
        vmm_get_mapping(vmm_get_kernel_page_table(), virt, &mapped_phys, &mapped_flags);
        raw_id = ioapic_read_with_echo(slot, IOAPIC_REG_ID, &id_echo);
        raw_ver = ioapic_read_with_echo(slot, IOAPIC_REG_VERSION, &ver_echo);
        redir_count = ((raw_ver >> 16) & 0xFFu) + 1u;

        g_ioapics[slot].present = true;
        g_ioapics[slot].madt_id = madt->ioapics[i].id;
        g_ioapics[slot].gsi_base = madt->ioapics[i].gsi_base;
        g_ioapics[slot].phys = madt->ioapics[i].address;
        g_ioapics[slot].mapped_phys = mapped_phys;
        g_ioapics[slot].mapped_flags = mapped_flags;
        hhdm_virt = (uint64_t)(uintptr_t)hhdm_phys_to_virt(madt->ioapics[i].address);
        if (vmm_get_mapping(vmm_get_kernel_page_table(), hhdm_virt, &hhdm_phys, &hhdm_flags)) {
            volatile uint8_t* old_mmio = g_ioapic_mmio[slot];
            g_ioapic_mmio[slot] = (volatile uint8_t*)hhdm_virt;
            g_ioapics[slot].hhdm_mapped = true;
            g_ioapics[slot].hhdm_raw_version =
                ioapic_read(slot, IOAPIC_REG_VERSION);
            g_ioapic_mmio[slot] = old_mmio;
            (void)hhdm_phys;
            (void)hhdm_flags;
        }
        g_ioapics[slot].raw_id = raw_id;
        g_ioapics[slot].raw_version = raw_ver;
        g_ioapics[slot].id_select_echo = id_echo;
        g_ioapics[slot].version_select_echo = ver_echo;
        g_ioapics[slot].mmio_id = (raw_id >> 24) & 0x0Fu;
        g_ioapics[slot].version = raw_ver & 0xFFu;
        g_ioapics[slot].redir_count = redir_count;

        redirs_sampled = redir_count;
        if (redirs_sampled > APIC_MAX_IOAPIC_REDIRS) {
            redirs_sampled = APIC_MAX_IOAPIC_REDIRS;
        }
        g_ioapics[slot].redirs_sampled = redirs_sampled;
        for (uint32_t redir = 0; redir < redirs_sampled; redir++) {
            sample_redir_entry(slot, redir);
        }

        log_okf("apic",
                "IOAPIC %u mapped phys=%x map=%x flags=%x id=%u raw_ver=%x echo=%x version=%x redirs=%u gsi_base=%u",
                slot,
                g_ioapics[slot].phys,
                (uint32_t)g_ioapics[slot].mapped_phys,
                (uint32_t)g_ioapics[slot].mapped_flags,
                g_ioapics[slot].mmio_id,
                g_ioapics[slot].raw_version,
                g_ioapics[slot].version_select_echo,
                g_ioapics[slot].version,
                g_ioapics[slot].redir_count,
                g_ioapics[slot].gsi_base);
    }

    g_apic_probe_available = g_lapic.present || g_ioapic_count > 0u;
}

bool apic_probe_available(void) {
    return g_apic_probe_available;
}

const lapic_probe_info_t* apic_lapic_info(void) {
    return &g_lapic;
}

uint32_t apic_ioapic_count(void) {
    return g_ioapic_count;
}

const ioapic_probe_info_t* apic_ioapic_info(uint32_t index) {
    if (index >= g_ioapic_count) {
        return NULL;
    }

    return &g_ioapics[index];
}

void apic_lapic_eoi(void) {
    if (g_lapic.present) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

bool apic_legacy_irq_to_gsi(uint8_t irq, uint32_t* out_gsi, uint16_t* out_flags) {
    return acpi_irq_to_gsi(irq, out_gsi, out_flags);
}

bool apic_gsi_to_ioapic(uint32_t gsi, uint32_t* out_index, uint32_t* out_redir) {
    for (uint32_t i = 0; i < g_ioapic_count; i++) {
        uint32_t base = g_ioapics[i].gsi_base;
        uint32_t limit = base + g_ioapics[i].redir_count;

        if (!g_ioapics[i].present || limit < base) {
            continue;
        }
        if (gsi >= base && gsi < limit) {
            if (out_index) {
                *out_index = i;
            }
            if (out_redir) {
                *out_redir = gsi - base;
            }
            return true;
        }
    }

    return false;
}

bool apic_ioapic_read_redir(uint32_t gsi, uint64_t* out_entry) {
    uint32_t index = 0;
    uint32_t redir = 0;
    uint32_t low = 0;
    uint32_t high = 0;

    if (!out_entry || !apic_gsi_to_ioapic(gsi, &index, &redir)) {
        return false;
    }

    low = ioapic_read(index, IOAPIC_REG_REDIR_BASE + redir * 2u);
    high = ioapic_read(index, IOAPIC_REG_REDIR_BASE + redir * 2u + 1u);
    *out_entry = ((uint64_t)high << 32) | low;
    return true;
}

bool apic_ioapic_write_redir(uint32_t gsi, uint64_t entry) {
    uint32_t index = 0;
    uint32_t redir = 0;

    if (!apic_gsi_to_ioapic(gsi, &index, &redir)) {
        return false;
    }

    ioapic_write(index, IOAPIC_REG_REDIR_BASE + redir * 2u + 1u, (uint32_t)(entry >> 32));
    ioapic_write(index, IOAPIC_REG_REDIR_BASE + redir * 2u, (uint32_t)entry);
    return true;
}

bool apic_ioapic_set_mask(uint32_t gsi, bool masked) {
    uint64_t entry = 0;

    if (!apic_ioapic_read_redir(gsi, &entry)) {
        return false;
    }

    if (masked) {
        entry |= IOAPIC_REDIR_MASKED;
    } else {
        entry &= ~(uint64_t)IOAPIC_REDIR_MASKED;
    }

    return apic_ioapic_write_redir(gsi, entry);
}

void apic_dump(struct limine_framebuffer* fb) {
    print(fb, "APIC probe: ");
    print(fb, g_apic_probe_available ? "available\n" : "not available\n");

    if (g_lapic.present) {
        print(fb, "LAPIC: phys=");
        print_hex(fb, g_lapic.phys);
        print(fb, " id=");
        print_u32(fb, g_lapic.id);
        print(fb, " version=");
        print_hex(fb, g_lapic.version);
        print(fb, " max_lvt=");
        print_u32(fb, g_lapic.max_lvt);
        print(fb, " svr=");
        print_hex(fb, g_lapic.svr);
        print(fb, "\n");
    } else {
        print(fb, "LAPIC: not mapped\n");
    }

    print(fb, "IOAPICs: ");
    print_u32(fb, g_ioapic_count);
    print(fb, "\n");
    for (uint32_t i = 0; i < g_ioapic_count; i++) {
        print(fb, "  ");
        print_u32(fb, i);
        print(fb, ": phys=");
        print_hex(fb, g_ioapics[i].phys);
        print(fb, " madt_id=");
        print_u32(fb, g_ioapics[i].madt_id);
        print(fb, " mmio_id=");
        print_u32(fb, g_ioapics[i].mmio_id);
        print(fb, " raw_id=");
        print_hex(fb, g_ioapics[i].raw_id);
        print(fb, " raw_ver=");
        print_hex(fb, g_ioapics[i].raw_version);
        print(fb, " echo=");
        print_hex(fb, g_ioapics[i].version_select_echo);
        print(fb, " version=");
        print_hex(fb, g_ioapics[i].version);
        print(fb, " redirs=");
        print_u32(fb, g_ioapics[i].redir_count);
        print(fb, " gsi_base=");
        print_u32(fb, g_ioapics[i].gsi_base);
        print(fb, "\n");
        print(fb, "    map_phys=");
        print_hex(fb, g_ioapics[i].mapped_phys);
        print(fb, " map_flags=");
        print_hex(fb, g_ioapics[i].mapped_flags);
        print(fb, " hhdm=");
        print(fb, g_ioapics[i].hhdm_mapped ? "yes" : "no");
        if (g_ioapics[i].hhdm_mapped) {
            print(fb, " hhdm_raw_ver=");
            print_hex(fb, g_ioapics[i].hhdm_raw_version);
        }
        print(fb, "\n");

        for (uint32_t redir = 0; redir < g_ioapics[i].redirs_sampled; redir++) {
            const ioapic_redir_probe_info_t* entry = &g_ioapics[i].redirs[redir];
            print(fb, "    gsi=");
            print_u32(fb, entry->gsi);
            print(fb, " vec=");
            print_hex(fb, entry->vector);
            print(fb, " dest=");
            print_u32(fb, entry->dest);
            print(fb, entry->masked ? " masked" : " unmasked");
            print(fb, entry->trigger_level ? " level" : " edge");
            print(fb, entry->polarity_low ? " low" : " high");
            print(fb, " dm=");
            print_u32(fb, entry->delivery_mode);
            print(fb, " raw=");
            print_hex(fb, ((uint64_t)entry->high << 32) | entry->low);
            print(fb, "\n");
        }
    }
}
