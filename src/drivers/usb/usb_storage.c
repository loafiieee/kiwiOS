#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86/io.h"
#include "core/log.h"
#include "drivers/usb/usb_storage.h"
#include "libc/string.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"

#define UHCI_MAX_CONTROLLERS 4u
#define UHCI_MAX_STORAGE_DEVS 8u
#define UHCI_PORTS_PER_CONTROLLER 2u

#define UHCI_REG_USBCMD  0x00
#define UHCI_REG_USBSTS  0x02
#define UHCI_REG_USBINTR 0x04
#define UHCI_REG_FRNUM   0x06
#define UHCI_REG_FLBASE  0x08
#define UHCI_REG_SOFMOD  0x0c
#define UHCI_REG_PORTSC1 0x10

#define UHCI_CMD_RS      (1u << 0)
#define UHCI_CMD_HCRESET (1u << 1)
#define UHCI_CMD_CF      (1u << 6)
#define UHCI_CMD_MAXP    (1u << 7)

#define UHCI_PORT_CCS    (1u << 0)
#define UHCI_PORT_CSC    (1u << 1)
#define UHCI_PORT_PE     (1u << 2)
#define UHCI_PORT_PEC    (1u << 3)
#define UHCI_PORT_LSDA   (1u << 8)
#define UHCI_PORT_RESET  (1u << 9)

#define UHCI_PTR_TERM    0x00000001u
#define UHCI_PTR_QH      0x00000002u

#define UHCI_TD_ACTIVE   (1u << 23)
#define UHCI_TD_IOC      (1u << 24)
#define UHCI_TD_LS       (1u << 26)
#define UHCI_TD_ERRCNT   (3u << 27)
#define UHCI_TD_ERR_MASK 0x007f0000u

#define USB_PID_OUT      0xe1u
#define USB_PID_IN       0x69u
#define USB_PID_SETUP    0x2du

#define USB_REQ_GET_DESCRIPTOR  6u
#define USB_REQ_SET_ADDRESS     5u
#define USB_REQ_SET_CONFIG      9u

#define USB_DESC_DEVICE         1u
#define USB_DESC_CONFIG         2u
#define USB_DESC_INTERFACE      4u
#define USB_DESC_ENDPOINT       5u

#define USB_CLASS_MASS_STORAGE  0x08u
#define USB_SUBCLASS_SCSI       0x06u
#define USB_PROTOCOL_BOT        0x50u

#define USB_ENDPOINT_IN         0x80u
#define USB_ENDPOINT_XFER_BULK  0x02u

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t link;
    volatile uint32_t element;
} uhci_qh_t;

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t link;
    volatile uint32_t ctrl;
    volatile uint32_t token;
    volatile uint32_t buffer;
} uhci_td_t;

typedef struct __attribute__((packed)) {
    uint8_t bm_request_type;
    uint8_t b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
} usb_setup_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t len;
    uint8_t type;
    uint16_t usb_bcd;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t max_packet0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device_bcd;
    uint8_t manufacturer;
    uint8_t product_str;
    uint8_t serial;
    uint8_t config_count;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t len;
    uint8_t type;
    uint16_t total_length;
    uint8_t interfaces;
    uint8_t config_value;
    uint8_t config_str;
    uint8_t attrs;
    uint8_t max_power;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t len;
    uint8_t type;
    uint8_t number;
    uint8_t alternate;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_str;
} usb_interface_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t len;
    uint8_t type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usb_endpoint_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t cdb[16];
} usb_msc_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;
} usb_msc_csw_t;

typedef struct {
    bool present;
    uint16_t io_base;
    uint64_t frame_list_phys;
    volatile uint32_t* frame_list;
    int8_t port_dev[UHCI_PORTS_PER_CONTROLLER];
} uhci_controller_t;

typedef struct {
    bool present;
    uhci_controller_t* hc;
    uint8_t port;
    uint8_t address;
    uint8_t max_packet0;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
    uint64_t total_sectors;
    uint32_t block_size;
    uint32_t tag;
} usb_storage_dev_t;

static uhci_controller_t g_uhci[UHCI_MAX_CONTROLLERS];
static uint32_t g_uhci_count = 0;
static usb_storage_dev_t g_storage[UHCI_MAX_STORAGE_DEVS];
static uint32_t g_storage_count = 0;
static uint8_t g_next_usb_address = 1;

static void tiny_delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) {
        asm volatile("pause");
    }
}

static uint16_t uhci_port_reg(uint8_t port) {
    return (uint16_t)(UHCI_REG_PORTSC1 + ((uint16_t)port * 2u));
}

static uint32_t uhci_td_token(uint8_t pid,
                              uint8_t addr,
                              uint8_t endpoint,
                              uint8_t toggle,
                              uint16_t max_len) {
    uint32_t encoded_len = (max_len == 0u) ? 0x7ffu : ((uint32_t)max_len - 1u);

    return (uint32_t)pid |
           ((uint32_t)(addr & 0x7fu) << 8) |
           ((uint32_t)(endpoint & 0x0fu) << 15) |
           ((uint32_t)(toggle & 1u) << 19) |
           ((encoded_len & 0x7ffu) << 21);
}

static bool dma_alloc_page(uint64_t* out_phys, uint8_t** out_virt) {
    void* phys = NULL;

    if (!out_phys || !out_virt) {
        return false;
    }

    phys = pmm_alloc();
    if (!phys) {
        return false;
    }

    *out_phys = (uint64_t)(uintptr_t)phys;
    *out_virt = (uint8_t*)hhdm_phys_to_virt(*out_phys);
    memset(*out_virt, 0, PAGE_SIZE);
    return true;
}

static bool uhci_submit(uhci_controller_t* hc,
                        uhci_qh_t* qh,
                        uint32_t qh_phys,
                        uhci_td_t* td,
                        uint32_t td_count,
                        bool low_speed) {
    bool done = false;

    if (!hc || !qh || !td || td_count == 0u || td_count > 128u) {
        return false;
    }

    qh->link = UHCI_PTR_TERM;
    qh->element = (uint32_t)((uintptr_t)td - (uintptr_t)qh + qh_phys);

    for (uint32_t i = 0; i < 1024u; i++) {
        hc->frame_list[i] = qh_phys | UHCI_PTR_QH;
    }

    (void)low_speed;

    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        done = true;
        for (uint32_t i = 0; i < td_count; i++) {
            if ((td[i].ctrl & UHCI_TD_ACTIVE) != 0u) {
                done = false;
                break;
            }
        }
        if (done) {
            break;
        }
        asm volatile("pause");
    }

    for (uint32_t i = 0; i < 1024u; i++) {
        hc->frame_list[i] = UHCI_PTR_TERM;
    }

    if (!done) {
        log_error("usb", "UHCI transfer timed out");
        return false;
    }

    for (uint32_t i = 0; i < td_count; i++) {
        if ((td[i].ctrl & UHCI_TD_ERR_MASK) != 0u) {
            log_errorf("usb", "UHCI TD error ctrl=%x token=%x",
                       td[i].ctrl,
                       td[i].token);
            return false;
        }
    }

    return true;
}

static bool uhci_control(uhci_controller_t* hc,
                         uint8_t addr,
                         uint8_t max_packet,
                         uint8_t request_type,
                         uint8_t request,
                         uint16_t value,
                         uint16_t index,
                         void* data,
                         uint16_t len) {
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    uhci_qh_t* qh = NULL;
    uhci_td_t* td = NULL;
    usb_setup_packet_t* setup = NULL;
    uint8_t* xfer_data = NULL;
    uint32_t qh_phys = 0;
    uint32_t td_phys = 0;
    uint32_t setup_phys = 0;
    uint32_t data_phys = 0;
    uint32_t td_count = 0;
    uint16_t remaining = len;
    uint16_t offset = 0;
    uint8_t toggle = 1;
    bool is_in = (request_type & 0x80u) != 0u;
    bool ok = false;

    if (!hc || max_packet == 0u || len > 512u) {
        return false;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    qh = (uhci_qh_t*)page;
    td = (uhci_td_t*)(page + 32u);
    setup = (usb_setup_packet_t*)(page + 3072u);
    xfer_data = page + 3200u;

    qh_phys = (uint32_t)page_phys;
    td_phys = (uint32_t)(page_phys + 32u);
    setup_phys = (uint32_t)(page_phys + 3072u);
    data_phys = (uint32_t)(page_phys + 3200u);

    setup->bm_request_type = request_type;
    setup->b_request = request;
    setup->w_value = value;
    setup->w_index = index;
    setup->w_length = len;

    if (!is_in && data && len != 0u) {
        memcpy(xfer_data, data, len);
    }

    td[td_count].link = (td_phys + ((td_count + 1u) * sizeof(uhci_td_t)));
    td[td_count].ctrl = UHCI_TD_ACTIVE | UHCI_TD_ERRCNT;
    td[td_count].token = uhci_td_token(USB_PID_SETUP, addr, 0, 0, sizeof(*setup));
    td[td_count].buffer = setup_phys;
    td_count++;

    while (remaining > 0u) {
        uint16_t chunk = remaining;
        uint8_t pid = is_in ? USB_PID_IN : USB_PID_OUT;

        if (chunk > max_packet) {
            chunk = max_packet;
        }

        td[td_count].link = (td_phys + ((td_count + 1u) * sizeof(uhci_td_t)));
        td[td_count].ctrl = UHCI_TD_ACTIVE | UHCI_TD_ERRCNT;
        td[td_count].token = uhci_td_token(pid, addr, 0, toggle, chunk);
        td[td_count].buffer = data_phys + offset;
        td_count++;

        toggle ^= 1u;
        offset = (uint16_t)(offset + chunk);
        remaining = (uint16_t)(remaining - chunk);
    }

    td[td_count].link = UHCI_PTR_TERM;
    td[td_count].ctrl = UHCI_TD_ACTIVE | UHCI_TD_ERRCNT | UHCI_TD_IOC;
    td[td_count].token = uhci_td_token(is_in ? USB_PID_OUT : USB_PID_IN,
                                       addr,
                                       0,
                                       1,
                                       0);
    td[td_count].buffer = 0;
    td_count++;

    for (uint32_t i = 0; i + 1u < td_count; i++) {
        td[i].link = td_phys + ((i + 1u) * sizeof(uhci_td_t));
    }
    td[td_count - 1u].link = UHCI_PTR_TERM;

    ok = uhci_submit(hc, qh, qh_phys, td, td_count, false);
    if (ok && is_in && data && len != 0u) {
        memcpy(data, xfer_data, len);
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
}

static bool uhci_bulk(usb_storage_dev_t* dev,
                      uint8_t endpoint_addr,
                      bool in,
                      uint64_t data_phys,
                      uint32_t len,
                      uint16_t max_packet,
                      uint8_t* toggle) {
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    uhci_qh_t* qh = NULL;
    uhci_td_t* td = NULL;
    uint32_t qh_phys = 0;
    uint32_t td_phys = 0;
    uint32_t td_count = 0;
    uint32_t remaining = len;
    uint32_t offset = 0;
    bool ok = false;

    if (!dev || !dev->hc || !toggle || max_packet == 0u) {
        return false;
    }

    if (len == 0u) {
        remaining = 0u;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    qh = (uhci_qh_t*)page;
    td = (uhci_td_t*)(page + 32u);
    qh_phys = (uint32_t)page_phys;
    td_phys = (uint32_t)(page_phys + 32u);

    if (len == 0u) {
        td[0].link = UHCI_PTR_TERM;
        td[0].ctrl = UHCI_TD_ACTIVE | UHCI_TD_ERRCNT | UHCI_TD_IOC;
        td[0].token = uhci_td_token(in ? USB_PID_IN : USB_PID_OUT,
                                    dev->address,
                                    endpoint_addr & 0x0fu,
                                    *toggle,
                                    0);
        td[0].buffer = 0;
        td_count = 1u;
    } else {
        while (remaining > 0u && td_count < 128u) {
            uint16_t chunk = (remaining > max_packet) ? max_packet : (uint16_t)remaining;

            td[td_count].link = UHCI_PTR_TERM;
            td[td_count].ctrl = UHCI_TD_ACTIVE | UHCI_TD_ERRCNT;
            td[td_count].token = uhci_td_token(in ? USB_PID_IN : USB_PID_OUT,
                                               dev->address,
                                               endpoint_addr & 0x0fu,
                                               *toggle,
                                               chunk);
            td[td_count].buffer = (uint32_t)(data_phys + offset);

            if (td_count > 0u) {
                td[td_count - 1u].link = td_phys + (td_count * sizeof(uhci_td_t));
            }

            *toggle ^= 1u;
            offset += chunk;
            remaining -= chunk;
            td_count++;
        }

        if (remaining != 0u) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }
    }

    td[td_count - 1u].ctrl |= UHCI_TD_IOC;
    ok = uhci_submit(dev->hc, qh, qh_phys, td, td_count, false);

    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static bool msc_command_dma(usb_storage_dev_t* dev,
                            const uint8_t* cdb,
                            uint8_t cdb_len,
                            uint64_t data_phys,
                            uint32_t data_len,
                            bool data_in) {
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    usb_msc_cbw_t* cbw = NULL;
    usb_msc_csw_t* csw = NULL;
    uint32_t tag = 0;
    bool ok = false;

    if (!dev || !cdb || cdb_len == 0u || cdb_len > 16u) {
        return false;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    cbw = (usb_msc_cbw_t*)page;
    csw = (usb_msc_csw_t*)(page + 64u);

    tag = ++dev->tag;
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = tag;
    cbw->data_transfer_length = data_len;
    cbw->flags = data_in ? 0x80u : 0x00u;
    cbw->lun = 0;
    cbw->cdb_length = cdb_len;
    memcpy(cbw->cdb, cdb, cdb_len);

    ok = uhci_bulk(dev,
                   dev->bulk_out_ep,
                   false,
                   page_phys,
                   sizeof(*cbw),
                   dev->bulk_out_max_packet,
                   &dev->bulk_out_toggle);
    if (!ok) goto out;

    if (data_len != 0u) {
        ok = uhci_bulk(dev,
                       data_in ? dev->bulk_in_ep : dev->bulk_out_ep,
                       data_in,
                       data_phys,
                       data_len,
                       data_in ? dev->bulk_in_max_packet : dev->bulk_out_max_packet,
                       data_in ? &dev->bulk_in_toggle : &dev->bulk_out_toggle);
        if (!ok) goto out;
    }

    memset(csw, 0, sizeof(*csw));
    ok = uhci_bulk(dev,
                   dev->bulk_in_ep,
                   true,
                   page_phys + 64u,
                   sizeof(*csw),
                   dev->bulk_in_max_packet,
                   &dev->bulk_in_toggle);
    if (!ok) goto out;

    if (csw->signature != CSW_SIGNATURE || csw->tag != tag || csw->status != 0u) {
        log_errorf("usb", "MSC command failed sig=%x tag=%x status=%u",
                   csw->signature,
                   csw->tag,
                   csw->status);
        ok = false;
    }

out:
    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
}

static bool msc_command_small(usb_storage_dev_t* dev,
                              const uint8_t* cdb,
                              uint8_t cdb_len,
                              void* data,
                              uint32_t data_len,
                              bool data_in) {
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    bool ok = false;

    if (data_len > PAGE_SIZE) {
        return false;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    if (!data_in && data && data_len != 0u) {
        memcpy(page, data, data_len);
    }

    ok = msc_command_dma(dev, cdb, cdb_len, page_phys, data_len, data_in);
    if (ok && data_in && data && data_len != 0u) {
        memcpy(data, page, data_len);
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
}

static bool msc_read_capacity(usb_storage_dev_t* dev) {
    uint8_t cdb[10];
    uint8_t data[8];
    uint32_t last_lba = 0;
    uint32_t block_size = 0;

    memset(cdb, 0, sizeof(cdb));
    memset(data, 0, sizeof(data));
    cdb[0] = 0x25u;

    if (!msc_command_small(dev, cdb, sizeof(cdb), data, sizeof(data), true)) {
        return false;
    }

    last_lba = be32(data);
    block_size = be32(data + 4u);
    if (block_size != 512u) {
        log_errorf("usb", "USB storage block size %u unsupported", block_size);
        return false;
    }

    dev->block_size = block_size;
    dev->total_sectors = (uint64_t)last_lba + 1u;
    return true;
}

static bool parse_mass_storage_config(const uint8_t* cfg,
                                      uint16_t cfg_len,
                                      uint8_t* out_config_value,
                                      uint8_t* out_bulk_in,
                                      uint16_t* out_bulk_in_mps,
                                      uint8_t* out_bulk_out,
                                      uint16_t* out_bulk_out_mps) {
    uint16_t off = 0;
    bool in_mass_storage = false;

    if (!cfg || cfg_len < sizeof(usb_config_desc_t)) {
        return false;
    }

    *out_config_value = ((const usb_config_desc_t*)cfg)->config_value;
    *out_bulk_in = 0;
    *out_bulk_out = 0;

    while (off + 2u <= cfg_len) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1u];

        if (len < 2u || off + len > cfg_len) {
            break;
        }

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t* iface = (const usb_interface_desc_t*)(cfg + off);
            in_mass_storage =
                iface->interface_class == USB_CLASS_MASS_STORAGE &&
                iface->interface_subclass == USB_SUBCLASS_SCSI &&
                iface->interface_protocol == USB_PROTOCOL_BOT;
        } else if (type == USB_DESC_ENDPOINT && in_mass_storage && len >= sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t* ep = (const usb_endpoint_desc_t*)(cfg + off);
            if ((ep->attributes & 0x03u) == USB_ENDPOINT_XFER_BULK) {
                if ((ep->endpoint_address & USB_ENDPOINT_IN) != 0u) {
                    *out_bulk_in = ep->endpoint_address;
                    *out_bulk_in_mps = ep->max_packet_size;
                } else {
                    *out_bulk_out = ep->endpoint_address;
                    *out_bulk_out_mps = ep->max_packet_size;
                }
            }
        }

        off = (uint16_t)(off + len);
    }

    return *out_bulk_in != 0u && *out_bulk_out != 0u &&
           *out_bulk_in_mps != 0u && *out_bulk_out_mps != 0u;
}

static bool enumerate_storage_on_port(uhci_controller_t* hc, uint8_t port) {
    uint16_t port_reg = uhci_port_reg(port);
    uint16_t status = 0;
    usb_device_desc_t dev_desc;
    uint8_t cfg_first[9];
    uint8_t cfg_full[512];
    uint16_t cfg_len = 0;
    uint8_t address = 0;
    uint8_t config_value = 0;
    uint8_t bulk_in = 0;
    uint8_t bulk_out = 0;
    uint16_t bulk_in_mps = 0;
    uint16_t bulk_out_mps = 0;
    uint8_t max_packet0 = 8;
    usb_storage_dev_t* dev = NULL;

    if (!hc || g_storage_count >= UHCI_MAX_STORAGE_DEVS) {
        return false;
    }

    status = inw((uint16_t)(hc->io_base + port_reg));
    if ((status & UHCI_PORT_CCS) == 0u || (status & UHCI_PORT_LSDA) != 0u) {
        return false;
    }

    outw((uint16_t)(hc->io_base + port_reg), (uint16_t)(status | UHCI_PORT_RESET));
    tiny_delay(400000u);
    status = (uint16_t)(inw((uint16_t)(hc->io_base + port_reg)) & ~UHCI_PORT_RESET);
    outw((uint16_t)(hc->io_base + port_reg), status);
    tiny_delay(200000u);
    status = inw((uint16_t)(hc->io_base + port_reg));
    outw((uint16_t)(hc->io_base + port_reg), (uint16_t)(status | UHCI_PORT_PE | UHCI_PORT_CSC | UHCI_PORT_PEC));
    tiny_delay(200000u);

    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!uhci_control(hc,
                      0,
                      8,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_DEVICE << 8),
                      0,
                      &dev_desc,
                      8)) {
        return false;
    }

    if (dev_desc.max_packet0 == 0u) {
        dev_desc.max_packet0 = 8u;
    }
    max_packet0 = dev_desc.max_packet0;

    address = g_next_usb_address++;
    if (address == 0u || address > 120u) {
        address = 1u;
        g_next_usb_address = 2u;
    }

    if (!uhci_control(hc,
                      0,
                      max_packet0,
                      0x00u,
                      USB_REQ_SET_ADDRESS,
                      address,
                      0,
                      NULL,
                      0)) {
        return false;
    }
    tiny_delay(200000u);

    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!uhci_control(hc,
                      address,
                      max_packet0,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_DEVICE << 8),
                      0,
                      &dev_desc,
                      sizeof(dev_desc))) {
        return false;
    }

    memset(cfg_first, 0, sizeof(cfg_first));
    if (!uhci_control(hc,
                      address,
                      max_packet0,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_CONFIG << 8),
                      0,
                      cfg_first,
                      sizeof(cfg_first))) {
        return false;
    }

    cfg_len = ((const usb_config_desc_t*)cfg_first)->total_length;
    if (cfg_len < sizeof(usb_config_desc_t) || cfg_len > sizeof(cfg_full)) {
        return false;
    }

    memset(cfg_full, 0, sizeof(cfg_full));
    if (!uhci_control(hc,
                      address,
                      max_packet0,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_CONFIG << 8),
                      0,
                      cfg_full,
                      cfg_len)) {
        return false;
    }

    if (!parse_mass_storage_config(cfg_full,
                                   cfg_len,
                                   &config_value,
                                   &bulk_in,
                                   &bulk_in_mps,
                                   &bulk_out,
                                   &bulk_out_mps)) {
        return false;
    }

    if (!uhci_control(hc,
                      address,
                      max_packet0,
                      0x00u,
                      USB_REQ_SET_CONFIG,
                      config_value,
                      0,
                      NULL,
                      0)) {
        return false;
    }
    tiny_delay(200000u);

    dev = &g_storage[g_storage_count];
    memset(dev, 0, sizeof(*dev));
    dev->present = true;
    dev->hc = hc;
    dev->port = port;
    dev->address = address;
    dev->max_packet0 = max_packet0;
    dev->bulk_in_ep = bulk_in;
    dev->bulk_out_ep = bulk_out;
    dev->bulk_in_max_packet = bulk_in_mps;
    dev->bulk_out_max_packet = bulk_out_mps;
    dev->bulk_in_toggle = 0;
    dev->bulk_out_toggle = 0;
    dev->tag = 0x1000u + g_storage_count;

    if (!msc_read_capacity(dev)) {
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    hc->port_dev[port] = (int8_t)g_storage_count;
    g_storage_count++;
    log_okf("usb", "USB mass storage disk detected: addr=%u sectors=%x",
            address,
            (uint32_t)dev->total_sectors);
    return true;
}

void uhci_probe_pci(uint8_t bus, uint8_t dev, uint8_t func, uint16_t io_base) {
    uhci_controller_t* hc = NULL;
    uint64_t frame_phys = 0;
    uint8_t* frame_virt = NULL;

    (void)bus;
    (void)dev;
    (void)func;

    if (io_base == 0u) {
        return;
    }

    for (uint32_t i = 0; i < g_uhci_count; i++) {
        if (g_uhci[i].present && g_uhci[i].io_base == io_base) {
            return;
        }
    }

    if (g_uhci_count >= UHCI_MAX_CONTROLLERS) {
        log_error("usb", "UHCI controller limit reached");
        return;
    }

    if (!dma_alloc_page(&frame_phys, &frame_virt)) {
        log_error("usb", "failed to allocate UHCI frame list");
        return;
    }

    hc = &g_uhci[g_uhci_count];
    memset(hc, 0, sizeof(*hc));
    hc->present = true;
    hc->io_base = io_base;
    hc->frame_list_phys = frame_phys;
    hc->frame_list = (volatile uint32_t*)frame_virt;
    for (uint32_t i = 0; i < UHCI_PORTS_PER_CONTROLLER; i++) {
        hc->port_dev[i] = -1;
    }
    for (uint32_t i = 0; i < 1024u; i++) {
        hc->frame_list[i] = UHCI_PTR_TERM;
    }

    outw((uint16_t)(io_base + UHCI_REG_USBCMD), 0);
    tiny_delay(10000u);
    outw((uint16_t)(io_base + UHCI_REG_USBCMD), UHCI_CMD_HCRESET);
    tiny_delay(100000u);
    outl((uint16_t)(io_base + UHCI_REG_FLBASE), (uint32_t)frame_phys);
    outw((uint16_t)(io_base + UHCI_REG_FRNUM), 0);
    outb((uint16_t)(io_base + UHCI_REG_SOFMOD), 0x40u);
    outw((uint16_t)(io_base + UHCI_REG_USBINTR), 0);
    outw((uint16_t)(io_base + UHCI_REG_USBSTS), 0x003fu);
    outw((uint16_t)(io_base + UHCI_REG_USBCMD), UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);

    g_uhci_count++;
    log_okf("usb", "UHCI controller registered io=%x", io_base);
}

uint32_t usb_storage_rescan(void) {
    uint32_t added = 0;

    for (uint32_t i = 0; i < g_uhci_count; i++) {
        uhci_controller_t* hc = &g_uhci[i];
        if (!hc->present) {
            continue;
        }

        for (uint8_t port = 0; port < UHCI_PORTS_PER_CONTROLLER; port++) {
            uint16_t status = inw((uint16_t)(hc->io_base + uhci_port_reg(port)));

            if ((status & UHCI_PORT_CCS) == 0u) {
                hc->port_dev[port] = -1;
                outw((uint16_t)(hc->io_base + uhci_port_reg(port)),
                     (uint16_t)(status | UHCI_PORT_CSC | UHCI_PORT_PEC));
                continue;
            }

            if (hc->port_dev[port] >= 0) {
                continue;
            }

            if (enumerate_storage_on_port(hc, port)) {
                added++;
            }
        }
    }

    return added;
}

uint32_t usb_storage_disk_count(void) {
    return g_storage_count;
}

uint64_t usb_storage_total_sectors(uint32_t index) {
    if (index >= g_storage_count || !g_storage[index].present) {
        return 0;
    }
    return g_storage[index].total_sectors;
}

bool usb_storage_read(uint32_t index, uint64_t lba, uint32_t sector_count, void* buffer) {
    usb_storage_dev_t* dev = NULL;
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    uint32_t done = 0;

    if (index >= g_storage_count || !buffer || sector_count == 0u) {
        return false;
    }

    dev = &g_storage[index];
    if (!dev->present || dev->block_size != 512u) {
        return false;
    }
    if (lba >= dev->total_sectors || (uint64_t)sector_count > (dev->total_sectors - lba)) {
        return false;
    }
    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    while (done < sector_count) {
        uint32_t chunk = sector_count - done;
        uint32_t max_chunk = ((uint32_t)dev->bulk_in_max_packet * 120u) / 512u;
        uint8_t cdb[10];

        if (max_chunk == 0u) {
            max_chunk = 1u;
        }
        if (max_chunk > 8u) {
            max_chunk = 8u;
        }
        if (chunk > max_chunk) {
            chunk = max_chunk;
        }
        if ((lba + done) > 0xffffffffu) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x28u;
        put_be32(cdb + 2u, (uint32_t)(lba + done));
        put_be16(cdb + 7u, (uint16_t)chunk);

        memset(page, 0, PAGE_SIZE);
        if (!msc_command_dma(dev, cdb, sizeof(cdb), page_phys, chunk * 512u, true)) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        memcpy((uint8_t*)buffer + ((uint64_t)done * 512u), page, chunk * 512u);
        done += chunk;
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return true;
}

bool usb_storage_write(uint32_t index, uint64_t lba, uint32_t sector_count, const void* buffer) {
    usb_storage_dev_t* dev = NULL;
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    uint32_t done = 0;

    if (index >= g_storage_count || !buffer || sector_count == 0u) {
        return false;
    }

    dev = &g_storage[index];
    if (!dev->present || dev->block_size != 512u) {
        return false;
    }
    if (lba >= dev->total_sectors || (uint64_t)sector_count > (dev->total_sectors - lba)) {
        return false;
    }
    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    while (done < sector_count) {
        uint32_t chunk = sector_count - done;
        uint32_t max_chunk = ((uint32_t)dev->bulk_out_max_packet * 120u) / 512u;
        uint8_t cdb[10];

        if (max_chunk == 0u) {
            max_chunk = 1u;
        }
        if (max_chunk > 8u) {
            max_chunk = 8u;
        }
        if (chunk > max_chunk) {
            chunk = max_chunk;
        }
        if ((lba + done) > 0xffffffffu) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        memcpy(page, (const uint8_t*)buffer + ((uint64_t)done * 512u), chunk * 512u);

        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x2au;
        put_be32(cdb + 2u, (uint32_t)(lba + done));
        put_be16(cdb + 7u, (uint16_t)chunk);

        if (!msc_command_dma(dev, cdb, sizeof(cdb), page_phys, chunk * 512u, false)) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        done += chunk;
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return true;
}

bool usb_storage_flush(uint32_t index) {
    uint8_t cdb[10];

    if (index >= g_storage_count || !g_storage[index].present) {
        return false;
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35u;
    return msc_command_small(&g_storage[index], cdb, sizeof(cdb), NULL, 0, false);
}
