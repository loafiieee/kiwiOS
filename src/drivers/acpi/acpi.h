#ifndef DRIVERS_ACPI_ACPI_H
#define DRIVERS_ACPI_ACPI_H

#include <stdbool.h>
#include <stdint.h>
#include "limine.h"

#define ACPI_MAX_LAPICS 64u
#define ACPI_MAX_IOAPICS 8u
#define ACPI_MAX_ISOS 16u
#define ACPI_MAX_LAPIC_NMIS 16u
#define ACPI_MAX_MCFG_ENTRIES 8u

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct {
    uint32_t acpi_id;
    uint32_t apic_id;
    uint32_t flags;
    bool x2apic;
} acpi_lapic_info_t;

typedef struct {
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
} acpi_ioapic_info_t;

typedef struct {
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} acpi_iso_info_t;

typedef struct {
    uint32_t acpi_id;
    uint8_t lint;
    uint16_t flags;
    bool all_processors;
} acpi_lapic_nmi_info_t;

typedef struct {
    bool present;
    uint64_t lapic_address;
    uint32_t flags;
    uint32_t lapic_count;
    uint32_t enabled_lapic_count;
    uint32_t ioapic_count;
    uint32_t iso_count;
    uint32_t lapic_nmi_count;
    acpi_lapic_info_t lapics[ACPI_MAX_LAPICS];
    acpi_ioapic_info_t ioapics[ACPI_MAX_IOAPICS];
    acpi_iso_info_t isos[ACPI_MAX_ISOS];
    acpi_lapic_nmi_info_t lapic_nmis[ACPI_MAX_LAPIC_NMIS];
} acpi_madt_info_t;

typedef struct __attribute__((packed)) {
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} acpi_gas_t;

typedef struct {
    bool present;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint32_t flags;
    uint16_t iapc_boot_arch;
    uint8_t century_register;
    bool reset_supported;
    acpi_gas_t reset_reg;
    uint8_t reset_value;
    uint64_t dsdt_address;
    acpi_gas_t pm1a_cnt_reg;
    acpi_gas_t pm1b_cnt_reg;
    bool s5_supported;
    uint16_t s5_slp_typa;
    uint16_t s5_slp_typb;
} acpi_fadt_info_t;

typedef struct {
    bool present;
    uint32_t event_timer_block_id;
    acpi_gas_t address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} acpi_hpet_info_t;

typedef struct {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t start_bus;
    uint8_t end_bus;
} acpi_mcfg_entry_info_t;

typedef struct {
    bool present;
    uint32_t entry_count;
    acpi_mcfg_entry_info_t entries[ACPI_MAX_MCFG_ENTRIES];
} acpi_mcfg_info_t;

void acpi_init(struct limine_rsdp_response* response);
bool acpi_available(void);
const acpi_sdt_header_t* acpi_find_table(const char signature[4]);
uint32_t acpi_table_count(void);
const acpi_sdt_header_t* acpi_table_at(uint32_t index);
const acpi_madt_info_t* acpi_madt_info(void);
const acpi_fadt_info_t* acpi_fadt_info(void);
const acpi_hpet_info_t* acpi_hpet_info(void);
const acpi_mcfg_info_t* acpi_mcfg_info(void);
bool acpi_irq_to_gsi(uint8_t irq, uint32_t* out_gsi, uint16_t* out_flags);
bool acpi_reboot(void);
bool acpi_poweroff(void);
void acpi_dump(struct limine_framebuffer* fb);

#endif // DRIVERS_ACPI_ACPI_H
