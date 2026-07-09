#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_CONTROLLER_UHCI = 0,
    USB_CONTROLLER_OHCI,
    USB_CONTROLLER_EHCI,
    USB_CONTROLLER_XHCI,
    USB_CONTROLLER_UNKNOWN,
} usb_controller_type_t;

typedef struct {
    usb_controller_type_t type;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint64_t base;
    bool mmio;
    bool supported;
} usb_controller_info_t;

typedef bool (*usb_bulk_transfer_fn)(void* ctx,
                                     uint8_t endpoint_addr,
                                     bool in,
                                     uint64_t data_phys,
                                     uint32_t len,
                                     uint16_t max_packet);

typedef struct {
    const char* transport_name;
    void* ctx;
    uint8_t address;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    usb_bulk_transfer_fn bulk_transfer;
} usb_bot_transport_t;

void usb_note_controller(usb_controller_type_t type,
                         uint8_t bus,
                         uint8_t dev,
                         uint8_t func,
                         uint64_t base,
                         bool mmio,
                         bool supported);
const char* usb_controller_type_name(usb_controller_type_t type);
uint32_t usb_controller_count(void);
bool usb_controller_info(uint32_t index, usb_controller_info_t* out);
bool usb_storage_register_bot(const usb_bot_transport_t* transport, uint32_t* out_index);
bool usb_storage_is_present(uint32_t index);
void usb_storage_mark_removed(uint32_t index);

void uhci_probe_pci(uint8_t bus, uint8_t dev, uint8_t func, uint16_t io_base);

uint32_t usb_storage_rescan(void);
uint32_t usb_storage_full_rescan(void);
uint32_t usb_storage_disk_count(void);
uint64_t usb_storage_total_sectors(uint32_t index);
bool usb_storage_read(uint32_t index, uint64_t lba, uint32_t sector_count, void* buffer);
bool usb_storage_write(uint32_t index, uint64_t lba, uint32_t sector_count, const void* buffer);
bool usb_storage_flush(uint32_t index);
