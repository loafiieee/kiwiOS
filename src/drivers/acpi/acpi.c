#include "drivers/acpi/acpi.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86/io.h"
#include "core/console.h"
#include "core/log.h"
#include "libc/string.h"
#include "memory/vmm.h"

#define ACPI_MAX_TABLES 32u
#define ACPI_MAP_BASE 0xFFFFFFFFA3000000ull
#define ACPI_MAP_PAGES 1024u

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} acpi_rsdp_v1_t;

typedef struct __attribute__((packed)) {
    acpi_rsdp_v1_t v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_v2_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint32_t lapic_address;
    uint32_t flags;
} acpi_madt_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} acpi_madt_entry_header_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t hdr;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} acpi_madt_lapic_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t hdr;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} acpi_madt_ioapic_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t hdr;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} acpi_madt_iso_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t hdr;
    uint8_t acpi_processor_id;
    uint16_t flags;
    uint8_t lint;
} acpi_madt_lapic_nmi_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t hdr;
    uint16_t reserved;
    uint64_t lapic_address;
} acpi_madt_lapic_addr_override_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t hdr;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_uid;
} acpi_madt_x2apic_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    acpi_gas_t reset_reg;
    uint8_t reset_value;
    uint8_t reserved2[3];
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    acpi_gas_t x_pm1a_evt_blk;
    acpi_gas_t x_pm1b_evt_blk;
    acpi_gas_t x_pm1a_cnt_blk;
    acpi_gas_t x_pm1b_cnt_blk;
    acpi_gas_t x_pm2_cnt_blk;
    acpi_gas_t x_pm_tmr_blk;
    acpi_gas_t x_gpe0_blk;
    acpi_gas_t x_gpe1_blk;
} acpi_fadt_min_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint32_t event_timer_block_id;
    acpi_gas_t address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} acpi_hpet_table_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint64_t reserved;
} acpi_mcfg_t;

typedef struct __attribute__((packed)) {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} acpi_mcfg_entry_t;

#define FADT_FLAG_RESET_REG_SUPPORTED (1u << 10)
#define ACPI_GAS_SYSTEM_MEMORY 0u
#define ACPI_GAS_SYSTEM_IO 1u
#define ACPI_PM1_CNT_SCI_EN 0x0001u
#define ACPI_PM1_CNT_SLP_TYP_SHIFT 10u
#define ACPI_PM1_CNT_SLP_TYP_MASK (0x7u << ACPI_PM1_CNT_SLP_TYP_SHIFT)
#define ACPI_PM1_CNT_SLP_EN (1u << 13)

static bool g_acpi_available = false;
static const acpi_sdt_header_t* g_tables[ACPI_MAX_TABLES];
static uint32_t g_table_count = 0;
static const acpi_sdt_header_t* g_rsdt = NULL;
static const acpi_sdt_header_t* g_xsdt = NULL;
static uint32_t g_next_map_page = 0;
static acpi_madt_info_t g_madt;
static acpi_fadt_info_t g_fadt;
static acpi_hpet_info_t g_hpet;
static acpi_mcfg_info_t g_mcfg;

#define FIELD_PRESENT(type, field, len) \
    ((len) >= ((uint32_t)offsetof(type, field) + (uint32_t)sizeof(((type*)0)->field)))

static void* acpi_map_region_flags(uint64_t phys, uint32_t len, uint64_t flags) {
    page_table_t* kpt = vmm_get_kernel_page_table();
    uint64_t start = PAGE_ALIGN_DOWN(phys);
    uint64_t end = 0;
    uint32_t pages = 0;
    uint64_t virt = 0;

    if (!kpt || len == 0) {
        return NULL;
    }

    if (phys + len < phys) {
        return NULL;
    }

    end = PAGE_ALIGN_UP(phys + len);
    pages = (uint32_t)((end - start) / PAGE_SIZE);
    if (pages == 0 || g_next_map_page + pages > ACPI_MAP_PAGES) {
        log_error("acpi", "ACPI mapping window exhausted");
        return NULL;
    }

    virt = ACPI_MAP_BASE + ((uint64_t)g_next_map_page * PAGE_SIZE);
    for (uint32_t i = 0; i < pages; i++) {
        if (!vmm_map_page(kpt,
                          virt + ((uint64_t)i * PAGE_SIZE),
                          start + ((uint64_t)i * PAGE_SIZE),
                          flags)) {
            log_error("acpi", "failed to map ACPI physical page");
            return NULL;
        }
    }

    g_next_map_page += pages;
    return (void*)(virt + (phys - start));
}

static void* acpi_map_region(uint64_t phys, uint32_t len) {
    return acpi_map_region_flags(phys, len, 0);
}

static uint8_t acpi_checksum(const void* data, uint32_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum;
}

static bool signature_is(const char actual[4], const char expected[4]) {
    return actual[0] == expected[0] &&
           actual[1] == expected[1] &&
           actual[2] == expected[2] &&
           actual[3] == expected[3];
}

static void signature_to_string(const char signature[4], char out[5]) {
    out[0] = signature[0];
    out[1] = signature[1];
    out[2] = signature[2];
    out[3] = signature[3];
    out[4] = '\0';
}

static const acpi_sdt_header_t* map_sdt(uint64_t phys) {
    const acpi_sdt_header_t* hdr = NULL;

    if (phys == 0) {
        return NULL;
    }

    hdr = (const acpi_sdt_header_t*)acpi_map_region(phys, sizeof(acpi_sdt_header_t));
    if (!hdr || hdr->length < sizeof(*hdr)) {
        return NULL;
    }

    return (const acpi_sdt_header_t*)acpi_map_region(phys, hdr->length);
}

static bool validate_sdt(const acpi_sdt_header_t* hdr) {
    char sig[5];

    if (!hdr || hdr->length < sizeof(*hdr)) {
        return false;
    }

    if (acpi_checksum(hdr, hdr->length) != 0) {
        signature_to_string(hdr->signature, sig);
        log_errorf("acpi", "table %s checksum failed", sig);
        return false;
    }

    return true;
}

static void add_table(const acpi_sdt_header_t* hdr) {
    char sig[5];

    if (!validate_sdt(hdr)) {
        return;
    }

    signature_to_string(hdr->signature, sig);
    log_infof("acpi",
              "table %s len=%u rev=%u",
              sig,
              hdr->length,
              hdr->revision);

    if (g_table_count < ACPI_MAX_TABLES) {
        g_tables[g_table_count++] = hdr;
    } else {
        log_error("acpi", "table registry full");
    }
}

static bool parse_xsdt(const acpi_sdt_header_t* xsdt) {
    const uint64_t* entries = NULL;
    uint32_t count = 0;

    if (!validate_sdt(xsdt) || !signature_is(xsdt->signature, "XSDT")) {
        return false;
    }

    g_xsdt = xsdt;
    count = (xsdt->length - (uint32_t)sizeof(*xsdt)) / 8u;
    entries = (const uint64_t*)((const uint8_t*)xsdt + sizeof(*xsdt));
    log_okf("acpi", "XSDT found with %u table(s)", count);

    for (uint32_t i = 0; i < count; i++) {
        add_table(map_sdt(entries[i]));
    }

    return true;
}

static bool parse_rsdt(const acpi_sdt_header_t* rsdt) {
    const uint32_t* entries = NULL;
    uint32_t count = 0;

    if (!validate_sdt(rsdt) || !signature_is(rsdt->signature, "RSDT")) {
        return false;
    }

    g_rsdt = rsdt;
    count = (rsdt->length - (uint32_t)sizeof(*rsdt)) / 4u;
    entries = (const uint32_t*)((const uint8_t*)rsdt + sizeof(*rsdt));
    log_okf("acpi", "RSDT found with %u table(s)", count);

    for (uint32_t i = 0; i < count; i++) {
        add_table(map_sdt(entries[i]));
    }

    return true;
}

static const acpi_sdt_header_t* find_table_internal(const char signature[4]) {
    if (!signature) {
        return NULL;
    }

    for (uint32_t i = 0; i < g_table_count; i++) {
        if (signature_is(g_tables[i]->signature, signature)) {
            return g_tables[i];
        }
    }

    return NULL;
}

static void parse_madt(const acpi_sdt_header_t* hdr) {
    const acpi_madt_t* madt = (const acpi_madt_t*)hdr;
    const uint8_t* ptr = NULL;
    const uint8_t* end = NULL;

    if (!hdr || hdr->length < sizeof(acpi_madt_t)) {
        return;
    }

    g_madt.present = true;
    g_madt.lapic_address = madt->lapic_address;
    g_madt.flags = madt->flags;

    ptr = (const uint8_t*)madt + sizeof(acpi_madt_t);
    end = (const uint8_t*)madt + madt->hdr.length;

    while (ptr + sizeof(acpi_madt_entry_header_t) <= end) {
        const acpi_madt_entry_header_t* eh = (const acpi_madt_entry_header_t*)ptr;

        if (eh->length < sizeof(*eh) || ptr + eh->length > end) {
            log_error("acpi", "MADT entry has invalid length");
            break;
        }

        switch (eh->type) {
            case 0: {
                const acpi_madt_lapic_t* e = (const acpi_madt_lapic_t*)ptr;
                if (eh->length >= sizeof(*e) && g_madt.lapic_count < ACPI_MAX_LAPICS) {
                    acpi_lapic_info_t* out = &g_madt.lapics[g_madt.lapic_count++];
                    out->acpi_id = e->acpi_processor_id;
                    out->apic_id = e->apic_id;
                    out->flags = e->flags;
                    out->x2apic = false;
                    if ((e->flags & 1u) != 0u) {
                        g_madt.enabled_lapic_count++;
                    }
                }
                break;
            }
            case 1: {
                const acpi_madt_ioapic_t* e = (const acpi_madt_ioapic_t*)ptr;
                if (eh->length >= sizeof(*e) && g_madt.ioapic_count < ACPI_MAX_IOAPICS) {
                    acpi_ioapic_info_t* out = &g_madt.ioapics[g_madt.ioapic_count++];
                    out->id = e->ioapic_id;
                    out->address = e->address;
                    out->gsi_base = e->gsi_base;
                }
                break;
            }
            case 2: {
                const acpi_madt_iso_t* e = (const acpi_madt_iso_t*)ptr;
                if (eh->length >= sizeof(*e) && g_madt.iso_count < ACPI_MAX_ISOS) {
                    acpi_iso_info_t* out = &g_madt.isos[g_madt.iso_count++];
                    out->bus = e->bus;
                    out->source = e->source;
                    out->gsi = e->gsi;
                    out->flags = e->flags;
                }
                break;
            }
            case 4: {
                const acpi_madt_lapic_nmi_t* e = (const acpi_madt_lapic_nmi_t*)ptr;
                if (eh->length >= sizeof(*e) && g_madt.lapic_nmi_count < ACPI_MAX_LAPIC_NMIS) {
                    acpi_lapic_nmi_info_t* out = &g_madt.lapic_nmis[g_madt.lapic_nmi_count++];
                    out->acpi_id = e->acpi_processor_id;
                    out->flags = e->flags;
                    out->lint = e->lint;
                    out->all_processors = e->acpi_processor_id == 0xFFu;
                }
                break;
            }
            case 5: {
                const acpi_madt_lapic_addr_override_t* e =
                    (const acpi_madt_lapic_addr_override_t*)ptr;
                if (eh->length >= sizeof(*e)) {
                    g_madt.lapic_address = e->lapic_address;
                }
                break;
            }
            case 9: {
                const acpi_madt_x2apic_t* e = (const acpi_madt_x2apic_t*)ptr;
                if (eh->length >= sizeof(*e) && g_madt.lapic_count < ACPI_MAX_LAPICS) {
                    acpi_lapic_info_t* out = &g_madt.lapics[g_madt.lapic_count++];
                    out->acpi_id = e->acpi_uid;
                    out->apic_id = e->x2apic_id;
                    out->flags = e->flags;
                    out->x2apic = true;
                    if ((e->flags & 1u) != 0u) {
                        g_madt.enabled_lapic_count++;
                    }
                }
                break;
            }
            default:
                break;
        }

        ptr += eh->length;
    }

    log_okf("acpi",
            "MADT parsed: cpus=%u enabled=%u ioapics=%u iso=%u nmi=%u",
            g_madt.lapic_count,
            g_madt.enabled_lapic_count,
            g_madt.ioapic_count,
            g_madt.iso_count,
            g_madt.lapic_nmi_count);
}


static uint8_t gas_access_size_from_bytes(uint8_t bytes) {
    switch (bytes) {
        case 1: return 1;
        case 2: return 2;
        case 4: return 3;
        case 8: return 4;
        default: return 0;
    }
}

static acpi_gas_t legacy_io_gas(uint32_t address, uint8_t bytes) {
    acpi_gas_t gas;

    memset(&gas, 0, sizeof(gas));
    if (bytes == 0) {
        bytes = 2;
    }
    if (bytes > 8) {
        bytes = 8;
    }

    gas.address_space_id = ACPI_GAS_SYSTEM_IO;
    gas.register_bit_width = (uint8_t)(bytes * 8u);
    gas.register_bit_offset = 0;
    gas.access_size = gas_access_size_from_bytes(bytes);
    gas.address = address;
    return gas;
}

static bool aml_parse_pkg_length(const uint8_t* ptr,
                                 const uint8_t* end,
                                 const uint8_t** out_payload,
                                 uint32_t* out_payload_len) {
    const uint8_t* start = ptr;
    uint8_t lead = 0;
    uint8_t following = 0;
    uint32_t total_len = 0;
    uint32_t header_len = 0;

    if (!ptr || ptr >= end || !out_payload || !out_payload_len) {
        return false;
    }

    lead = *ptr++;
    following = (uint8_t)(lead >> 6);
    if ((uint32_t)(end - ptr) < following) {
        return false;
    }

    if (following == 0) {
        total_len = lead & 0x3Fu;
    } else {
        total_len = lead & 0x0Fu;
        for (uint8_t i = 0; i < following; i++) {
            total_len |= (uint32_t)ptr[i] << (4u + (8u * i));
        }
        ptr += following;
    }

    header_len = (uint32_t)(ptr - start);
    if (total_len < header_len || (uint32_t)(end - start) < total_len) {
        return false;
    }

    *out_payload = ptr;
    *out_payload_len = total_len - header_len;
    return true;
}

static bool aml_parse_integer(const uint8_t** ptr, const uint8_t* end, uint64_t* out_value) {
    const uint8_t* p = ptr ? *ptr : NULL;

    if (!p || p >= end || !out_value) {
        return false;
    }

    switch (*p++) {
        case 0x00:
            *out_value = 0;
            break;
        case 0x01:
            *out_value = 1;
            break;
        case 0x0A:
            if (p + 1 > end) {
                return false;
            }
            *out_value = *p++;
            break;
        case 0x0B:
            if (p + 2 > end) {
                return false;
            }
            *out_value = (uint64_t)p[0] | ((uint64_t)p[1] << 8);
            p += 2;
            break;
        case 0x0C:
            if (p + 4 > end) {
                return false;
            }
            *out_value = (uint64_t)p[0] |
                         ((uint64_t)p[1] << 8) |
                         ((uint64_t)p[2] << 16) |
                         ((uint64_t)p[3] << 24);
            p += 4;
            break;
        case 0x0E:
            if (p + 8 > end) {
                return false;
            }
            *out_value = (uint64_t)p[0] |
                         ((uint64_t)p[1] << 8) |
                         ((uint64_t)p[2] << 16) |
                         ((uint64_t)p[3] << 24) |
                         ((uint64_t)p[4] << 32) |
                         ((uint64_t)p[5] << 40) |
                         ((uint64_t)p[6] << 48) |
                         ((uint64_t)p[7] << 56);
            p += 8;
            break;
        default:
            return false;
    }

    *ptr = p;
    return true;
}

static void parse_s5_from_dsdt(const acpi_sdt_header_t* dsdt) {
    const uint8_t* aml = NULL;
    const uint8_t* end = NULL;

    g_fadt.s5_supported = false;
    g_fadt.s5_slp_typa = 0;
    g_fadt.s5_slp_typb = 0;

    if (!dsdt || dsdt->length <= sizeof(*dsdt) || !signature_is(dsdt->signature, "DSDT")) {
        return;
    }
    if (!validate_sdt(dsdt)) {
        return;
    }

    aml = (const uint8_t*)dsdt + sizeof(*dsdt);
    end = (const uint8_t*)dsdt + dsdt->length;

    for (const uint8_t* p = aml; p + 4 < end; p++) {
        const uint8_t* pkg = NULL;
        const uint8_t* payload = NULL;
        const uint8_t* payload_end = NULL;
        uint32_t payload_len = 0;
        uint64_t slp_typa = 0;
        uint64_t slp_typb = 0;

        if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_') {
            continue;
        }

        if (p > aml && p[-1] == '\\') {
            if (p < aml + 2 || p[-2] != 0x08) {
                continue;
            }
        } else if (p == aml || p[-1] != 0x08) {
            continue;
        }

        pkg = p + 4;
        if (pkg >= end || *pkg != 0x12) {
            continue;
        }
        pkg++;

        if (!aml_parse_pkg_length(pkg, end, &payload, &payload_len) || payload_len < 1u) {
            continue;
        }

        payload_end = payload + payload_len;
        payload++;
        if (!aml_parse_integer(&payload, payload_end, &slp_typa)) {
            continue;
        }
        if (!aml_parse_integer(&payload, payload_end, &slp_typb)) {
            slp_typb = slp_typa;
        }

        if (slp_typa > 7u || slp_typb > 7u) {
            continue;
        }

        g_fadt.s5_supported = true;
        g_fadt.s5_slp_typa = (uint16_t)slp_typa;
        g_fadt.s5_slp_typb = (uint16_t)slp_typb;
        log_okf("acpi", "DSDT _S5 found: slp_typa=%u slp_typb=%u", g_fadt.s5_slp_typa, g_fadt.s5_slp_typb);
        return;
    }

    log_info("acpi", "DSDT _S5 package not found; ACPI poweroff unavailable");
}

static void parse_fadt(const acpi_sdt_header_t* hdr) {
    const acpi_fadt_min_t* fadt = (const acpi_fadt_min_t*)hdr;

    if (!hdr || hdr->length < offsetof(acpi_fadt_min_t, pm1a_evt_blk)) {
        return;
    }

    g_fadt.present = true;
    if (FIELD_PRESENT(acpi_fadt_min_t, sci_int, hdr->length)) {
        g_fadt.sci_int = fadt->sci_int;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, smi_cmd, hdr->length)) {
        g_fadt.smi_cmd = fadt->smi_cmd;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, acpi_enable, hdr->length)) {
        g_fadt.acpi_enable = fadt->acpi_enable;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, acpi_disable, hdr->length)) {
        g_fadt.acpi_disable = fadt->acpi_disable;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, dsdt, hdr->length)) {
        g_fadt.dsdt_address = fadt->dsdt;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, x_dsdt, hdr->length) && fadt->x_dsdt != 0) {
        g_fadt.dsdt_address = fadt->x_dsdt;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, pm1a_cnt_blk, hdr->length) && fadt->pm1a_cnt_blk != 0) {
        g_fadt.pm1a_cnt_reg = legacy_io_gas(fadt->pm1a_cnt_blk, fadt->pm1_cnt_len);
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, pm1b_cnt_blk, hdr->length) && fadt->pm1b_cnt_blk != 0) {
        g_fadt.pm1b_cnt_reg = legacy_io_gas(fadt->pm1b_cnt_blk, fadt->pm1_cnt_len);
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, x_pm1a_cnt_blk, hdr->length) && fadt->x_pm1a_cnt_blk.address != 0) {
        g_fadt.pm1a_cnt_reg = fadt->x_pm1a_cnt_blk;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, x_pm1b_cnt_blk, hdr->length) && fadt->x_pm1b_cnt_blk.address != 0) {
        g_fadt.pm1b_cnt_reg = fadt->x_pm1b_cnt_blk;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, iapc_boot_arch, hdr->length)) {
        g_fadt.iapc_boot_arch = fadt->iapc_boot_arch;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, century, hdr->length)) {
        g_fadt.century_register = fadt->century;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, flags, hdr->length)) {
        g_fadt.flags = fadt->flags;
    }
    if (FIELD_PRESENT(acpi_fadt_min_t, reset_value, hdr->length)) {
        g_fadt.reset_supported = (g_fadt.flags & FADT_FLAG_RESET_REG_SUPPORTED) != 0u;
        g_fadt.reset_reg = fadt->reset_reg;
        g_fadt.reset_value = fadt->reset_value;
    }

    if (g_fadt.dsdt_address != 0) {
        parse_s5_from_dsdt(map_sdt(g_fadt.dsdt_address));
    }

    log_okf("acpi",
            "FADT parsed: sci=%u smi=%x flags=%x reset=%u s5=%u century=%x",
            g_fadt.sci_int,
            g_fadt.smi_cmd,
            g_fadt.flags,
            g_fadt.reset_supported ? 1u : 0u,
            g_fadt.s5_supported ? 1u : 0u,
            g_fadt.century_register);
}

static void parse_hpet(const acpi_sdt_header_t* hdr) {
    const acpi_hpet_table_t* hpet = (const acpi_hpet_table_t*)hdr;

    if (!hdr || hdr->length < sizeof(*hpet)) {
        return;
    }

    g_hpet.present = true;
    g_hpet.event_timer_block_id = hpet->event_timer_block_id;
    g_hpet.address = hpet->address;
    g_hpet.hpet_number = hpet->hpet_number;
    g_hpet.minimum_tick = hpet->minimum_tick;
    g_hpet.page_protection = hpet->page_protection;

    log_okf("acpi",
            "HPET parsed: id=%x addr_hi=%x addr_lo=%x min_tick=%u",
            g_hpet.event_timer_block_id,
            (uint32_t)(g_hpet.address.address >> 32),
            (uint32_t)g_hpet.address.address,
            g_hpet.minimum_tick);
}

static void parse_mcfg(const acpi_sdt_header_t* hdr) {
    const acpi_mcfg_t* mcfg = (const acpi_mcfg_t*)hdr;
    const acpi_mcfg_entry_t* entries = NULL;
    uint32_t count = 0;

    if (!hdr || hdr->length < sizeof(*mcfg)) {
        return;
    }

    g_mcfg.present = true;
    count = (hdr->length - (uint32_t)sizeof(*mcfg)) / (uint32_t)sizeof(acpi_mcfg_entry_t);
    entries = (const acpi_mcfg_entry_t*)((const uint8_t*)mcfg + sizeof(*mcfg));

    for (uint32_t i = 0; i < count && i < ACPI_MAX_MCFG_ENTRIES; i++) {
        g_mcfg.entries[g_mcfg.entry_count].base_address = entries[i].base_address;
        g_mcfg.entries[g_mcfg.entry_count].segment_group = entries[i].segment_group;
        g_mcfg.entries[g_mcfg.entry_count].start_bus = entries[i].start_bus;
        g_mcfg.entries[g_mcfg.entry_count].end_bus = entries[i].end_bus;
        g_mcfg.entry_count++;
    }

    log_okf("acpi", "MCFG parsed: entries=%u", g_mcfg.entry_count);
}

static void parse_known_tables(void) {
    memset(&g_madt, 0, sizeof(g_madt));
    memset(&g_fadt, 0, sizeof(g_fadt));
    memset(&g_hpet, 0, sizeof(g_hpet));
    memset(&g_mcfg, 0, sizeof(g_mcfg));

    parse_madt(find_table_internal("APIC"));
    parse_fadt(find_table_internal("FACP"));
    parse_hpet(find_table_internal("HPET"));
    parse_mcfg(find_table_internal("MCFG"));
}

void acpi_init(struct limine_rsdp_response* response) {
    const acpi_rsdp_v2_t* rsdp = NULL;
    bool used_root_table = false;

    g_acpi_available = false;
    g_table_count = 0;
    g_rsdt = NULL;
    g_xsdt = NULL;
    g_next_map_page = 0;

    if (!response || response->address == 0) {
        log_error("acpi", "Limine did not provide RSDP");
        return;
    }

    rsdp = (const acpi_rsdp_v2_t*)acpi_map_region(response->address, sizeof(acpi_rsdp_v2_t));
    if (!rsdp) {
        log_error("acpi", "failed to map RSDP");
        return;
    }
    if (memcmp(rsdp->v1.signature, "RSD PTR ", 8) != 0) {
        log_error("acpi", "RSDP signature invalid");
        return;
    }

    if (acpi_checksum(rsdp, sizeof(acpi_rsdp_v1_t)) != 0) {
        log_error("acpi", "RSDP v1 checksum failed");
        return;
    }

    if (rsdp->v1.revision >= 2u) {
        if (rsdp->length < sizeof(acpi_rsdp_v2_t)) {
            log_error("acpi", "RSDP v2 length invalid");
            return;
        }
        rsdp = (const acpi_rsdp_v2_t*)acpi_map_region(response->address, rsdp->length);
        if (!rsdp) {
            log_error("acpi", "failed to map full RSDP");
            return;
        }
        if (acpi_checksum(rsdp, rsdp->length) != 0) {
            log_error("acpi", "RSDP v2 checksum failed");
            return;
        }
    }

    log_okf("acpi",
            "RSDP valid rev=%u rsdt=%x xsdt_hi=%x xsdt_lo=%x",
            rsdp->v1.revision,
            rsdp->v1.rsdt_address,
            (uint32_t)(((rsdp->v1.revision >= 2u) ? rsdp->xsdt_address : 0u) >> 32),
            (uint32_t)((rsdp->v1.revision >= 2u) ? rsdp->xsdt_address : 0u));

    if (rsdp->v1.revision >= 2u && rsdp->xsdt_address != 0) {
        used_root_table = parse_xsdt(map_sdt(rsdp->xsdt_address));
    }

    if (!used_root_table && rsdp->v1.rsdt_address != 0) {
        used_root_table = parse_rsdt(map_sdt(rsdp->v1.rsdt_address));
    }

    if (!used_root_table) {
        log_error("acpi", "no valid XSDT/RSDT found");
        return;
    }

    parse_known_tables();
    g_acpi_available = true;
}

bool acpi_available(void) {
    return g_acpi_available;
}

const acpi_sdt_header_t* acpi_find_table(const char signature[4]) {
    return find_table_internal(signature);
}

uint32_t acpi_table_count(void) {
    return g_table_count;
}

const acpi_sdt_header_t* acpi_table_at(uint32_t index) {
    if (index >= g_table_count) {
        return NULL;
    }

    return g_tables[index];
}

const acpi_madt_info_t* acpi_madt_info(void) {
    return &g_madt;
}

const acpi_fadt_info_t* acpi_fadt_info(void) {
    return &g_fadt;
}

const acpi_hpet_info_t* acpi_hpet_info(void) {
    return &g_hpet;
}

const acpi_mcfg_info_t* acpi_mcfg_info(void) {
    return &g_mcfg;
}

bool acpi_irq_to_gsi(uint8_t irq, uint32_t* out_gsi, uint16_t* out_flags) {
    if (!g_madt.present) {
        return false;
    }

    for (uint32_t i = 0; i < g_madt.iso_count; i++) {
        if (g_madt.isos[i].bus == 0 && g_madt.isos[i].source == irq) {
            if (out_gsi) {
                *out_gsi = g_madt.isos[i].gsi;
            }
            if (out_flags) {
                *out_flags = g_madt.isos[i].flags;
            }
            return true;
        }
    }

    if (out_gsi) {
        *out_gsi = irq;
    }
    if (out_flags) {
        *out_flags = 0;
    }
    return true;
}

static uint32_t gas_access_bytes(const acpi_gas_t* gas) {
    if (!gas) {
        return 0;
    }

    switch (gas->access_size) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 4;
        case 4: return 8;
        default:
            break;
    }

    if (gas->register_bit_width <= 8u) {
        return 1;
    }
    if (gas->register_bit_width <= 16u) {
        return 2;
    }
    if (gas->register_bit_width <= 32u) {
        return 4;
    }
    if (gas->register_bit_width <= 64u) {
        return 8;
    }
    return 0;
}

static bool gas_read_value(const acpi_gas_t* gas, uint64_t* out_value) {
    uint32_t bytes = gas_access_bytes(gas);

    if (!gas || gas->address == 0 || bytes == 0 || !out_value) {
        return false;
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_IO) {
        if (gas->address > 0xFFFFu) {
            return false;
        }

        switch (bytes) {
            case 1:
                *out_value = inb((uint16_t)gas->address);
                return true;
            case 2:
                *out_value = inw((uint16_t)gas->address);
                return true;
            case 4:
                *out_value = inl((uint16_t)gas->address);
                return true;
            default:
                return false;
        }
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_MEMORY) {
        void* ptr = acpi_map_region_flags(gas->address,
                                          bytes,
                                          PAGE_WRITE | PAGE_WRITETHROUGH | PAGE_CACHE_DISABLE);
        if (!ptr) {
            return false;
        }

        switch (bytes) {
            case 1:
                *out_value = *(volatile uint8_t*)ptr;
                return true;
            case 2:
                *out_value = *(volatile uint16_t*)ptr;
                return true;
            case 4:
                *out_value = *(volatile uint32_t*)ptr;
                return true;
            case 8:
                *out_value = *(volatile uint64_t*)ptr;
                return true;
            default:
                return false;
        }
    }

    return false;
}

static bool gas_write_value(const acpi_gas_t* gas, uint64_t value) {
    uint32_t bytes = gas_access_bytes(gas);

    if (!gas || gas->address == 0 || bytes == 0) {
        return false;
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_IO) {
        if (gas->address > 0xFFFFu) {
            return false;
        }

        switch (bytes) {
            case 1:
                outb((uint16_t)gas->address, (uint8_t)value);
                return true;
            case 2:
                outw((uint16_t)gas->address, (uint16_t)value);
                return true;
            case 4:
                outl((uint16_t)gas->address, (uint32_t)value);
                return true;
            default:
                return false;
        }
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_MEMORY) {
        void* ptr = acpi_map_region_flags(gas->address,
                                          bytes,
                                          PAGE_WRITE | PAGE_WRITETHROUGH | PAGE_CACHE_DISABLE);
        if (!ptr) {
            return false;
        }

        switch (bytes) {
            case 1:
                *(volatile uint8_t*)ptr = (uint8_t)value;
                return true;
            case 2:
                *(volatile uint16_t*)ptr = (uint16_t)value;
                return true;
            case 4:
                *(volatile uint32_t*)ptr = (uint32_t)value;
                return true;
            case 8:
                *(volatile uint64_t*)ptr = value;
                return true;
            default:
                return false;
        }
    }

    return false;
}

static bool gas_write_reset_value(const acpi_gas_t* gas, uint8_t value) {
    return gas_write_value(gas, value);
}

bool acpi_reboot(void) {
    if (!g_acpi_available || !g_fadt.present || !g_fadt.reset_supported) {
        return false;
    }

    if (!gas_write_reset_value(&g_fadt.reset_reg, g_fadt.reset_value)) {
        log_error("acpi", "FADT reset register write failed");
        return false;
    }

    for (volatile uint32_t i = 0; i < 1000000u; i++) {
        asm volatile("pause");
    }

    return true;
}

static bool acpi_pm1_sci_enabled(void) {
    uint64_t value = 0;

    if (!gas_read_value(&g_fadt.pm1a_cnt_reg, &value)) {
        return false;
    }

    return (value & ACPI_PM1_CNT_SCI_EN) != 0;
}

static bool acpi_enable_mode(void) {
    if (acpi_pm1_sci_enabled()) {
        return true;
    }

    if (g_fadt.smi_cmd == 0 || g_fadt.acpi_enable == 0 || g_fadt.smi_cmd > 0xFFFFu) {
        return false;
    }

    outb((uint16_t)g_fadt.smi_cmd, g_fadt.acpi_enable);
    for (volatile uint32_t i = 0; i < 100000u; i++) {
        if (acpi_pm1_sci_enabled()) {
            return true;
        }
        asm volatile("pause");
    }

    return false;
}

bool acpi_poweroff(void) {
    uint64_t pm1a = 0;
    uint64_t pm1b = 0;

    if (!g_acpi_available || !g_fadt.present || !g_fadt.s5_supported ||
        g_fadt.pm1a_cnt_reg.address == 0) {
        return false;
    }

    if (!acpi_enable_mode()) {
        log_error("acpi", "failed to enable ACPI mode for poweroff");
        return false;
    }

    (void)gas_read_value(&g_fadt.pm1a_cnt_reg, &pm1a);
    pm1a &= ~((uint64_t)ACPI_PM1_CNT_SLP_TYP_MASK | (uint64_t)ACPI_PM1_CNT_SLP_EN);
    pm1a |= ((uint64_t)g_fadt.s5_slp_typa << ACPI_PM1_CNT_SLP_TYP_SHIFT) | ACPI_PM1_CNT_SLP_EN;

    asm volatile("cli" ::: "memory");
    if (!gas_write_value(&g_fadt.pm1a_cnt_reg, pm1a)) {
        asm volatile("sti" ::: "memory");
        log_error("acpi", "PM1a control write failed");
        return false;
    }

    if (g_fadt.pm1b_cnt_reg.address != 0) {
        (void)gas_read_value(&g_fadt.pm1b_cnt_reg, &pm1b);
        pm1b &= ~((uint64_t)ACPI_PM1_CNT_SLP_TYP_MASK | (uint64_t)ACPI_PM1_CNT_SLP_EN);
        pm1b |= ((uint64_t)g_fadt.s5_slp_typb << ACPI_PM1_CNT_SLP_TYP_SHIFT) | ACPI_PM1_CNT_SLP_EN;
        (void)gas_write_value(&g_fadt.pm1b_cnt_reg, pm1b);
    }

    for (volatile uint32_t i = 0; i < 1000000u; i++) {
        asm volatile("pause");
    }
    asm volatile("sti" ::: "memory");

    return true;
}

static void dump_gas(struct limine_framebuffer* fb, const acpi_gas_t* gas) {
    print(fb, "space=");
    print_u32(fb, gas->address_space_id);
    print(fb, " width=");
    print_u32(fb, gas->register_bit_width);
    print(fb, " access=");
    print_u32(fb, gas->access_size);
    print(fb, " addr=");
    print_hex(fb, gas->address);
}

void acpi_dump(struct limine_framebuffer* fb) {
    print(fb, "ACPI: ");
    print(fb, g_acpi_available ? "available\n" : "not available\n");

    print(fb, "Tables: ");
    print_u32(fb, g_table_count);
    print(fb, "\n");
    for (uint32_t i = 0; i < g_table_count; i++) {
        char sig[5];
        signature_to_string(g_tables[i]->signature, sig);
        print(fb, "  ");
        print_u32(fb, i);
        print(fb, ": ");
        print(fb, sig);
        print(fb, " len=");
        print_u32(fb, g_tables[i]->length);
        print(fb, " rev=");
        print_u32(fb, g_tables[i]->revision);
        print(fb, "\n");
    }

    if (g_madt.present) {
        print(fb, "MADT: lapic=");
        print_hex(fb, g_madt.lapic_address);
        print(fb, " flags=");
        print_hex(fb, g_madt.flags);
        print(fb, " cpus=");
        print_u32(fb, g_madt.lapic_count);
        print(fb, " enabled=");
        print_u32(fb, g_madt.enabled_lapic_count);
        print(fb, " ioapics=");
        print_u32(fb, g_madt.ioapic_count);
        print(fb, " iso=");
        print_u32(fb, g_madt.iso_count);
        print(fb, "\n");

        for (uint32_t i = 0; i < g_madt.lapic_count; i++) {
            print(fb, "  CPU ");
            print_u32(fb, i);
            print(fb, ": acpi=");
            print_u32(fb, g_madt.lapics[i].acpi_id);
            print(fb, " apic=");
            print_u32(fb, g_madt.lapics[i].apic_id);
            print(fb, g_madt.lapics[i].x2apic ? " x2apic" : " lapic");
            print(fb, " flags=");
            print_hex(fb, g_madt.lapics[i].flags);
            print(fb, "\n");
        }

        for (uint32_t i = 0; i < g_madt.ioapic_count; i++) {
            print(fb, "  IOAPIC ");
            print_u32(fb, i);
            print(fb, ": id=");
            print_u32(fb, g_madt.ioapics[i].id);
            print(fb, " addr=");
            print_hex(fb, g_madt.ioapics[i].address);
            print(fb, " gsi_base=");
            print_u32(fb, g_madt.ioapics[i].gsi_base);
            print(fb, "\n");
        }

        for (uint32_t i = 0; i < g_madt.iso_count; i++) {
            print(fb, "  ISO ");
            print_u32(fb, i);
            print(fb, ": bus=");
            print_u32(fb, g_madt.isos[i].bus);
            print(fb, " irq=");
            print_u32(fb, g_madt.isos[i].source);
            print(fb, " gsi=");
            print_u32(fb, g_madt.isos[i].gsi);
            print(fb, " flags=");
            print_hex(fb, g_madt.isos[i].flags);
            print(fb, "\n");
        }
    } else {
        print(fb, "MADT: not present\n");
    }

    if (g_fadt.present) {
        print(fb, "FADT: sci=");
        print_u32(fb, g_fadt.sci_int);
        print(fb, " smi=");
        print_hex(fb, g_fadt.smi_cmd);
        print(fb, " flags=");
        print_hex(fb, g_fadt.flags);
        print(fb, " boot_arch=");
        print_hex(fb, g_fadt.iapc_boot_arch);
        print(fb, " century=");
        print_hex(fb, g_fadt.century_register);
        print(fb, " s5=");
        print(fb, g_fadt.s5_supported ? "yes" : "no");
        print(fb, " slp_typa=");
        print_u32(fb, g_fadt.s5_slp_typa);
        print(fb, " slp_typb=");
        print_u32(fb, g_fadt.s5_slp_typb);
        print(fb, "\n");
        print(fb, "  PM1a_CNT ");
        dump_gas(fb, &g_fadt.pm1a_cnt_reg);
        print(fb, "\n");
        if (g_fadt.pm1b_cnt_reg.address != 0) {
            print(fb, "  PM1b_CNT ");
            dump_gas(fb, &g_fadt.pm1b_cnt_reg);
            print(fb, "\n");
        }
        print(fb, "  reset_supported=");
        print(fb, g_fadt.reset_supported ? "yes " : "no ");
        dump_gas(fb, &g_fadt.reset_reg);
        print(fb, " value=");
        print_hex(fb, g_fadt.reset_value);
        print(fb, "\n");
    } else {
        print(fb, "FADT: not present\n");
    }

    if (g_hpet.present) {
        print(fb, "HPET: id=");
        print_hex(fb, g_hpet.event_timer_block_id);
        print(fb, " number=");
        print_u32(fb, g_hpet.hpet_number);
        print(fb, " min_tick=");
        print_u32(fb, g_hpet.minimum_tick);
        print(fb, " ");
        dump_gas(fb, &g_hpet.address);
        print(fb, "\n");
    } else {
        print(fb, "HPET: not present\n");
    }

    if (g_mcfg.present) {
        print(fb, "MCFG: entries=");
        print_u32(fb, g_mcfg.entry_count);
        print(fb, "\n");
        for (uint32_t i = 0; i < g_mcfg.entry_count; i++) {
            print(fb, "  ECAM ");
            print_u32(fb, i);
            print(fb, ": base=");
            print_hex(fb, g_mcfg.entries[i].base_address);
            print(fb, " seg=");
            print_u32(fb, g_mcfg.entries[i].segment_group);
            print(fb, " buses=");
            print_u32(fb, g_mcfg.entries[i].start_bus);
            print(fb, "-");
            print_u32(fb, g_mcfg.entries[i].end_bus);
            print(fb, "\n");
        }
    } else {
        print(fb, "MCFG: not present\n");
    }
}
