#ifndef ARCH_X86_HPET_H
#define ARCH_X86_HPET_H

#include <stdbool.h>
#include <stdint.h>

#include "limine.h"

typedef struct {
    bool present;
    bool mapped;
    uint64_t phys;
    uint8_t address_space;
    uint32_t table_id;
    uint64_t raw_caps;
    uint64_t raw_config;
    uint64_t counter;
    uint32_t revision;
    uint32_t vendor_id;
    uint32_t timer_count;
    uint32_t counter_period_fs;
    bool counter_64bit;
    bool legacy_replacement;
    bool enabled;
    bool legacy_route_enabled;
} hpet_probe_info_t;

void hpet_probe_from_acpi(void);
bool hpet_probe_available(void);
bool hpet_timekeeping_start(void);
uint64_t hpet_monotonic_ns(void);
const hpet_probe_info_t* hpet_info(void);
void hpet_dump(struct limine_framebuffer* fb);

#endif // ARCH_X86_HPET_H
