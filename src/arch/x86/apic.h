#ifndef ARCH_X86_APIC_H
#define ARCH_X86_APIC_H

#include <stdbool.h>
#include <stdint.h>
#include "limine.h"

#define APIC_MAX_IOAPICS 8u
#define APIC_MAX_IOAPIC_REDIRS 64u

typedef struct {
    bool present;
    uint64_t phys;
    uint32_t id;
    uint32_t version;
    uint32_t max_lvt;
    uint32_t svr;
} lapic_probe_info_t;

typedef struct {
    bool present;
    uint32_t gsi;
    uint32_t low;
    uint32_t high;
    uint8_t vector;
    uint8_t delivery_mode;
    uint8_t dest;
    bool masked;
    bool remote_irr;
    bool polarity_low;
    bool trigger_level;
} ioapic_redir_probe_info_t;

typedef struct {
    bool present;
    uint8_t madt_id;
    uint32_t gsi_base;
    uint32_t phys;
    uint64_t mapped_phys;
    uint64_t mapped_flags;
    bool hhdm_mapped;
    uint32_t hhdm_raw_version;
    uint32_t raw_id;
    uint32_t raw_version;
    uint32_t id_select_echo;
    uint32_t version_select_echo;
    uint32_t mmio_id;
    uint32_t version;
    uint32_t redir_count;
    uint32_t redirs_sampled;
    ioapic_redir_probe_info_t redirs[APIC_MAX_IOAPIC_REDIRS];
} ioapic_probe_info_t;

void apic_probe_from_acpi(void);
bool apic_probe_available(void);
const lapic_probe_info_t* apic_lapic_info(void);
uint32_t apic_ioapic_count(void);
const ioapic_probe_info_t* apic_ioapic_info(uint32_t index);
void apic_lapic_eoi(void);
bool apic_legacy_irq_to_gsi(uint8_t irq, uint32_t* out_gsi, uint16_t* out_flags);
bool apic_gsi_to_ioapic(uint32_t gsi, uint32_t* out_index, uint32_t* out_redir);
bool apic_ioapic_read_redir(uint32_t gsi, uint64_t* out_entry);
bool apic_ioapic_write_redir(uint32_t gsi, uint64_t entry);
bool apic_ioapic_set_mask(uint32_t gsi, bool masked);
void apic_dump(struct limine_framebuffer* fb);

#endif // ARCH_X86_APIC_H
