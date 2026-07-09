#include "arch/x86/hpet.h"

#include <stdint.h>

#include "core/console.h"
#include "core/log.h"
#include "drivers/acpi/acpi.h"
#include "libc/string.h"
#include "memory/vmm.h"

#define HPET_MMIO_VIRT 0xFFFFFFFFA4200000ull

#define HPET_REG_CAPS 0x000u
#define HPET_REG_CONFIG 0x010u
#define HPET_REG_COUNTER 0x0F0u

#define ACPI_GAS_SYSTEM_MEMORY 0u

static volatile uint8_t* g_hpet_mmio = NULL;
static hpet_probe_info_t g_hpet;

static bool hpet_map(uint64_t phys) {
    page_table_t* kpt = vmm_get_kernel_page_table();

    if (!kpt || phys == 0) {
        return false;
    }

    return vmm_map_page(kpt,
                        HPET_MMIO_VIRT,
                        PAGE_ALIGN_DOWN(phys),
                        PAGE_WRITE | PAGE_WRITETHROUGH | PAGE_CACHE_DISABLE);
}

static uint64_t hpet_read64(uint32_t reg) {
    if (!g_hpet_mmio) {
        return 0;
    }

    return *(volatile uint64_t*)(g_hpet_mmio + reg);
}

static void hpet_write64(uint32_t reg, uint64_t value) {
    if (!g_hpet_mmio) {
        return;
    }

    *(volatile uint64_t*)(g_hpet_mmio + reg) = value;
}

void hpet_probe_from_acpi(void) {
    const acpi_hpet_info_t* acpi_hpet = acpi_hpet_info();
    uint64_t caps = 0;
    uint64_t config = 0;

    memset(&g_hpet, 0, sizeof(g_hpet));
    g_hpet_mmio = NULL;

    if (!acpi_available() || !acpi_hpet || !acpi_hpet->present) {
        log_error("hpet", "ACPI HPET table unavailable; HPET probe skipped");
        return;
    }

    g_hpet.present = true;
    g_hpet.phys = acpi_hpet->address.address;
    g_hpet.address_space = acpi_hpet->address.address_space_id;
    g_hpet.table_id = acpi_hpet->event_timer_block_id;

    if (acpi_hpet->address.address_space_id != ACPI_GAS_SYSTEM_MEMORY) {
        log_errorf("hpet",
                   "unsupported HPET address space %u",
                   acpi_hpet->address.address_space_id);
        return;
    }

    if (!hpet_map(acpi_hpet->address.address)) {
        log_error("hpet", "failed to map HPET MMIO");
        return;
    }

    g_hpet_mmio = (volatile uint8_t*)(HPET_MMIO_VIRT +
                                      (acpi_hpet->address.address - PAGE_ALIGN_DOWN(acpi_hpet->address.address)));
    caps = hpet_read64(HPET_REG_CAPS);
    config = hpet_read64(HPET_REG_CONFIG);

    g_hpet.mapped = true;
    g_hpet.raw_caps = caps;
    g_hpet.raw_config = config;
    g_hpet.counter = hpet_read64(HPET_REG_COUNTER);
    g_hpet.revision = (uint32_t)(caps & 0xFFu);
    g_hpet.timer_count = (uint32_t)(((caps >> 8) & 0x1Fu) + 1u);
    g_hpet.counter_64bit = (caps & (1ull << 13)) != 0;
    g_hpet.legacy_replacement = (caps & (1ull << 15)) != 0;
    g_hpet.vendor_id = (uint32_t)((caps >> 16) & 0xFFFFu);
    g_hpet.counter_period_fs = (uint32_t)(caps >> 32);
    g_hpet.enabled = (config & 1u) != 0;
    g_hpet.legacy_route_enabled = (config & 2u) != 0;

    log_okf("hpet",
            "HPET mapped phys=%x vendor=%x rev=%u timers=%u period_fs=%u enabled=%u",
            (uint32_t)g_hpet.phys,
            g_hpet.vendor_id,
            g_hpet.revision,
            g_hpet.timer_count,
            g_hpet.counter_period_fs,
            g_hpet.enabled ? 1u : 0u);
}

bool hpet_probe_available(void) {
    return g_hpet.present && g_hpet.mapped;
}

const hpet_probe_info_t* hpet_info(void) {
    return &g_hpet;
}

static uint64_t hpet_counter_value(void) {
    uint64_t counter = hpet_read64(HPET_REG_COUNTER);

    if (!g_hpet.counter_64bit) {
        counter &= 0xFFFFFFFFull;
    }
    return counter;
}

uint64_t hpet_monotonic_ns(void) {
    uint64_t counter = hpet_counter_value();
    uint64_t period_fs = g_hpet.counter_period_fs;

    if (!hpet_probe_available() || period_fs == 0u) {
        return 0;
    }

    return (counter / 1000000ull) * period_fs + ((counter % 1000000ull) * period_fs) / 1000000ull;
}

bool hpet_timekeeping_start(void) {
    uint64_t config;

    if (!hpet_probe_available() || !g_hpet.counter_64bit || g_hpet.counter_period_fs == 0u) {
        return false;
    }

    config = hpet_read64(HPET_REG_CONFIG);
    if ((config & 1u) == 0u) {
        hpet_write64(HPET_REG_CONFIG, config | 1u);
        config = hpet_read64(HPET_REG_CONFIG);
    }

    g_hpet.raw_config = config;
    g_hpet.enabled = (config & 1u) != 0;
    g_hpet.legacy_route_enabled = (config & 2u) != 0;
    g_hpet.counter = hpet_counter_value();

    if (!g_hpet.enabled) {
        return false;
    }

    log_okf("hpet", "HPET main counter enabled for timekeeping counter=%x", (uint32_t)g_hpet.counter);
    return true;
}

void hpet_dump(struct limine_framebuffer* fb) {
    print(fb, "HPET probe: ");
    print(fb, hpet_probe_available() ? "available\n" : "not available\n");

    if (!g_hpet.present) {
        return;
    }

    print(fb, "HPET: phys=");
    print_hex(fb, g_hpet.phys);
    print(fb, " space=");
    print_u32(fb, g_hpet.address_space);
    print(fb, " table_id=");
    print_hex(fb, g_hpet.table_id);
    print(fb, "\n");

    if (!g_hpet.mapped) {
        print(fb, "  not mapped\n");
        return;
    }

    print(fb, "  vendor=");
    print_hex(fb, g_hpet.vendor_id);
    print(fb, " revision=");
    print_u32(fb, g_hpet.revision);
    print(fb, " timers=");
    print_u32(fb, g_hpet.timer_count);
    print(fb, g_hpet.counter_64bit ? " 64bit" : " 32bit");
    print(fb, g_hpet.legacy_replacement ? " legacy-capable" : " no-legacy");
    print(fb, "\n");

    print(fb, "  period_fs=");
    print_u32(fb, g_hpet.counter_period_fs);
    print(fb, " enabled=");
    print(fb, g_hpet.enabled ? "yes" : "no");
    print(fb, " legacy_route=");
    print(fb, g_hpet.legacy_route_enabled ? "yes" : "no");
    print(fb, "\n");

    print(fb, "  caps=");
    print_hex(fb, g_hpet.raw_caps);
    print(fb, " config=");
    print_hex(fb, g_hpet.raw_config);
    print(fb, " counter=");
    print_hex(fb, g_hpet.counter);
    print(fb, "\n");
}
