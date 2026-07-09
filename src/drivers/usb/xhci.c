#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/log.h"
#include "drivers/usb/usb_storage.h"
#include "drivers/usb/xhci.h"
#include "libc/string.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#define XHCI_MAX_CONTROLLERS 4u
#define XHCI_MAX_PORTS 32u
#define XHCI_MAX_STORAGE_DEVS 8u
#define XHCI_RING_TRBS 64u
#define XHCI_EVENT_TRBS 128u

#define XHCI_MMIO_VIRT_BASE 0xFFFFFFFFA1000000ull
#define XHCI_MMIO_SLOTS 4u
#define XHCI_MMIO_WINDOW_PAGES 16u

#define XHCI_CAP_CAPLENGTH  0x00u
#define XHCI_CAP_HCIVERSION 0x02u
#define XHCI_CAP_HCSPARAMS1 0x04u
#define XHCI_CAP_HCSPARAMS2 0x08u
#define XHCI_CAP_HCCPARAMS1 0x10u
#define XHCI_CAP_DBOFF      0x14u
#define XHCI_CAP_RTSOFF     0x18u

#define XHCI_OP_USBCMD 0x00u
#define XHCI_OP_USBSTS 0x04u
#define XHCI_OP_CRCR   0x18u
#define XHCI_OP_DCBAAP 0x30u
#define XHCI_OP_CONFIG 0x38u
#define XHCI_OP_PORTS  0x400u
#define XHCI_PORT_STRIDE 0x10u

#define XHCI_USBCMD_RS    (1u << 0)
#define XHCI_USBCMD_HCRST (1u << 1)
#define XHCI_USBSTS_HCH   (1u << 0)
#define XHCI_USBSTS_CNR   (1u << 11)

#define XHCI_PORTSC_CCS (1u << 0)
#define XHCI_PORTSC_PED (1u << 1)
#define XHCI_PORTSC_PR  (1u << 4)
#define XHCI_PORTSC_CHANGE_MASK 0x00fe0000u

#define XHCI_INTR0_OFFSET 0x20u
#define XHCI_INTR_ERSTSZ  0x08u
#define XHCI_INTR_ERSTBA  0x10u
#define XHCI_INTR_ERDP    0x18u

#define XHCI_TRB_TYPE_LINK 6u
#define XHCI_TRB_TYPE_ENABLE_SLOT 9u
#define XHCI_TRB_TYPE_DISABLE_SLOT 10u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11u
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_TYPE_NORMAL 1u
#define XHCI_TRB_TYPE_SETUP_STAGE 2u
#define XHCI_TRB_TYPE_DATA_STAGE 3u
#define XHCI_TRB_TYPE_STATUS_STAGE 4u
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32u
#define XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT 33u
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT 34u
#define XHCI_TRB_CYCLE (1u << 0)
#define XHCI_TRB_IOC   (1u << 5)
#define XHCI_TRB_IDT   (1u << 6)
#define XHCI_TRB_TC    (1u << 1)
#define XHCI_TRB_DIR   (1u << 16)

#define XHCI_COMPLETION_SUCCESS 1u
#define XHCI_COMPLETION_SHORT_PACKET 13u

#define XHCI_EXTCAP_LEGACY 1u
#define XHCI_USBLEGSUP_BIOS_OWNED (1u << 16)
#define XHCI_USBLEGSUP_OS_OWNED   (1u << 24)

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_CONFIG     9u

#define USB_DESC_DEVICE    1u
#define USB_DESC_CONFIG    2u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT  5u

#define USB_CLASS_MASS_STORAGE 0x08u
#define USB_SUBCLASS_SCSI      0x06u
#define USB_PROTOCOL_BOT       0x50u

#define USB_ENDPOINT_IN        0x80u
#define USB_ENDPOINT_XFER_BULK 0x02u

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t param_lo;
    volatile uint32_t param_hi;
    volatile uint32_t status;
    volatile uint32_t control;
} xhci_trb_t;

typedef struct __attribute__((packed, aligned(16))) {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} xhci_erst_entry_t;

typedef struct {
    xhci_trb_t* trbs;
    uint64_t phys;
    uint32_t count;
    uint32_t enqueue;
    uint8_t cycle;
} xhci_ring_t;

typedef struct {
    bool present;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint64_t mmio_phys;

    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* db;
    volatile uint8_t* rt;

    uint8_t cap_len;
    uint16_t version;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t ctx_size;
    uint32_t scratchpad_count;

    uint64_t dcbaa_phys;
    uint64_t* dcbaa;
    uint64_t scratchpad_array_phys;
    uint64_t* scratchpad_array;

    xhci_ring_t cmd_ring;
    uint64_t event_ring_phys;
    xhci_trb_t* event_ring;
    uint32_t event_dequeue;
    uint8_t event_cycle;
    uint64_t erst_phys;
    xhci_erst_entry_t* erst;

    int16_t port_state[XHCI_MAX_PORTS];
} xhci_controller_t;

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

typedef struct {
    bool present;
    xhci_controller_t* hc;
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint8_t bulk_in_dci;
    uint8_t bulk_out_dci;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    uint64_t dev_ctx_phys;
    uint8_t* dev_ctx;
    xhci_ring_t ep0_ring;
    xhci_ring_t bulk_in_ring;
    xhci_ring_t bulk_out_ring;
} xhci_storage_ctx_t;

static uint64_t g_xhci_phys_pages[XHCI_MMIO_SLOTS];
static xhci_controller_t g_xhci[XHCI_MAX_CONTROLLERS];
static uint32_t g_xhci_count = 0;
static xhci_storage_ctx_t g_xhci_storage[XHCI_MAX_STORAGE_DEVS];
static uint32_t g_xhci_storage_count = 0;

static uint8_t rd8(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint8_t*)(base + off);
}

static uint16_t rd16(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint16_t*)(base + off);
}

static uint32_t rd32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static void wr32(volatile uint8_t* base, uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(base + off) = value;
}

static void wr64(volatile uint8_t* base, uint32_t off, uint64_t value) {
    *(volatile uint32_t*)(base + off) = (uint32_t)value;
    *(volatile uint32_t*)(base + off + 4u) = (uint32_t)(value >> 32);
}

static volatile uint8_t* xhci_map_window(uint64_t mmio_phys) {
    page_table_t* kpt = vmm_get_kernel_page_table();
    uint64_t phys_page = PAGE_ALIGN_DOWN(mmio_phys);
    uint64_t off = mmio_phys - phys_page;

    for (uint32_t i = 0; i < XHCI_MMIO_SLOTS; i++) {
        if (g_xhci_phys_pages[i] == phys_page) {
            return (volatile uint8_t*)(XHCI_MMIO_VIRT_BASE +
                                       ((uint64_t)i * XHCI_MMIO_WINDOW_PAGES * PAGE_SIZE) +
                                       off);
        }
    }

    for (uint32_t i = 0; i < XHCI_MMIO_SLOTS; i++) {
        if (g_xhci_phys_pages[i] == 0) {
            uint64_t virt_base = XHCI_MMIO_VIRT_BASE +
                                 ((uint64_t)i * XHCI_MMIO_WINDOW_PAGES * PAGE_SIZE);

            for (uint32_t page = 0; page < XHCI_MMIO_WINDOW_PAGES; page++) {
                if (!vmm_map_page(kpt,
                                  virt_base + ((uint64_t)page * PAGE_SIZE),
                                  phys_page + ((uint64_t)page * PAGE_SIZE),
                                  PAGE_PRESENT | PAGE_WRITE)) {
                    log_error("xhci", "failed to map MMIO window");
                    return NULL;
                }
            }

            g_xhci_phys_pages[i] = phys_page;
            return (volatile uint8_t*)(virt_base + off);
        }
    }

    log_error("xhci", "MMIO map slots exhausted");
    return NULL;
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

static bool xhci_ring_alloc(xhci_ring_t* ring, uint32_t trb_count) {
    uint64_t phys = 0;
    uint8_t* virt = NULL;

    if (!ring || trb_count < 4u || trb_count > 256u) {
        return false;
    }

    if (!dma_alloc_page(&phys, &virt)) {
        return false;
    }

    memset(ring, 0, sizeof(*ring));
    ring->trbs = (xhci_trb_t*)virt;
    ring->phys = phys;
    ring->count = trb_count;
    ring->cycle = 1u;

    ring->trbs[trb_count - 1u].param_lo = (uint32_t)phys;
    ring->trbs[trb_count - 1u].param_hi = (uint32_t)(phys >> 32);
    ring->trbs[trb_count - 1u].control =
        (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | XHCI_TRB_CYCLE;
    return true;
}

static const char* xhci_speed_name(uint32_t speed) {
    switch (speed) {
        case 1: return "full";
        case 2: return "low";
        case 3: return "high";
        case 4: return "super";
        case 5: return "super+";
        default: return "unknown";
    }
}

static void xhci_clear_port_changes(xhci_controller_t* hc, uint32_t port) {
    uint32_t off = XHCI_OP_PORTS + ((port - 1u) * XHCI_PORT_STRIDE);
    uint32_t value = rd32(hc->op, off);
    wr32(hc->op, off, (value & ~XHCI_PORTSC_CHANGE_MASK) | XHCI_PORTSC_CHANGE_MASK);
}

static bool xhci_wait_status_set(xhci_controller_t* hc, uint32_t mask, bool want_set) {
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        bool is_set = (rd32(hc->op, XHCI_OP_USBSTS) & mask) != 0u;
        if (is_set == want_set) {
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

static void xhci_take_ownership(xhci_controller_t* hc, uint32_t hcc1) {
    uint32_t off = ((hcc1 >> 16) & 0xffffu) << 2;

    for (uint32_t depth = 0; off != 0u && depth < 32u; depth++) {
        uint32_t cap = rd32(hc->cap, off);
        uint32_t cap_id = cap & 0xffu;
        uint32_t next = ((cap >> 8) & 0xffu) << 2;

        if (cap_id == XHCI_EXTCAP_LEGACY) {
            if ((cap & XHCI_USBLEGSUP_BIOS_OWNED) != 0u) {
                wr32(hc->cap, off, cap | XHCI_USBLEGSUP_OS_OWNED);

                for (uint32_t spin = 0; spin < 1000000u; spin++) {
                    cap = rd32(hc->cap, off);
                    if ((cap & XHCI_USBLEGSUP_BIOS_OWNED) == 0u &&
                        (cap & XHCI_USBLEGSUP_OS_OWNED) != 0u) {
                        break;
                    }
                    asm volatile("pause");
                }
            }

            wr32(hc->cap, off + 4u, 0u);
            log_infof("xhci", "legacy ownership cap=%x value=%x", off, cap);
            return;
        }

        off = next;
    }
}

static uint32_t xhci_trb_type(const xhci_trb_t* trb) {
    return (trb->control >> 10) & 0x3fu;
}

static uint32_t xhci_completion_code(const xhci_trb_t* trb) {
    return (trb->status >> 24) & 0xffu;
}

static uint8_t* xhci_input_ctx_at(xhci_controller_t* hc, uint8_t* input_ctx, uint32_t index) {
    return input_ctx + ((uint64_t)index * hc->ctx_size);
}

static void ctx_write32(uint8_t* ctx, uint32_t dword, uint32_t value) {
    ((uint32_t*)ctx)[dword] = value;
}

static uint8_t xhci_dci_from_ep(uint8_t ep_addr) {
    uint8_t ep_num = ep_addr & 0x0fu;

    if (ep_num == 0u) {
        return 1u;
    }

    return (uint8_t)((ep_num * 2u) + ((ep_addr & USB_ENDPOINT_IN) ? 1u : 0u));
}

static xhci_trb_t* xhci_ring_prepare(xhci_ring_t* ring, uint64_t* out_phys, uint8_t* out_cycle) {
    xhci_trb_t* trb = NULL;

    if (!ring || !ring->trbs || ring->enqueue >= (ring->count - 1u)) {
        return NULL;
    }

    trb = &ring->trbs[ring->enqueue];
    memset(trb, 0, sizeof(*trb));
    if (out_phys) {
        *out_phys = ring->phys + ((uint64_t)ring->enqueue * sizeof(xhci_trb_t));
    }
    if (out_cycle) {
        *out_cycle = ring->cycle;
    }
    return trb;
}

static void xhci_ring_advance(xhci_ring_t* ring) {
    ring->enqueue++;
    if (ring->enqueue == (ring->count - 1u)) {
        ring->trbs[ring->count - 1u].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (ring->cycle ? XHCI_TRB_CYCLE : 0u);
        ring->enqueue = 0u;
        ring->cycle ^= 1u;
    }
}

static bool xhci_next_event(xhci_controller_t* hc, xhci_trb_t* out) {
    xhci_trb_t* ev = NULL;
    uint32_t control = 0;
    uint64_t erdp = 0;

    if (!hc || !out || !hc->event_ring) {
        return false;
    }

    ev = &hc->event_ring[hc->event_dequeue];
    control = ev->control;
    if (((control & XHCI_TRB_CYCLE) != 0u) != (hc->event_cycle != 0u)) {
        return false;
    }

    *out = *ev;
    hc->event_dequeue++;
    if (hc->event_dequeue == XHCI_EVENT_TRBS) {
        hc->event_dequeue = 0u;
        hc->event_cycle ^= 1u;
    }

    erdp = hc->event_ring_phys + ((uint64_t)hc->event_dequeue * sizeof(xhci_trb_t));
    wr64(hc->rt + XHCI_INTR0_OFFSET, XHCI_INTR_ERDP, erdp | (1ull << 3));
    return true;
}

static bool xhci_wait_command(xhci_controller_t* hc,
                              uint64_t cmd_phys,
                              xhci_trb_t* out_event,
                              uint32_t* out_code) {
    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        xhci_trb_t ev;
        if (xhci_next_event(hc, &ev)) {
            uint32_t type = xhci_trb_type(&ev);
            uint64_t ptr = ((uint64_t)ev.param_hi << 32) | ev.param_lo;

            if (type == XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT && ptr == cmd_phys) {
                uint32_t code = xhci_completion_code(&ev);
                if (out_event) {
                    *out_event = ev;
                }
                if (out_code) {
                    *out_code = code;
                }
                return code == XHCI_COMPLETION_SUCCESS;
            }
        }
        asm volatile("pause");
    }

    log_error("xhci", "command timed out");
    return false;
}

static bool xhci_wait_transfer(xhci_controller_t* hc,
                               uint8_t slot_id,
                               uint8_t dci,
                               xhci_trb_t* out_event,
                               uint32_t* out_code) {
    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        xhci_trb_t ev;
        if (xhci_next_event(hc, &ev)) {
            uint32_t type = xhci_trb_type(&ev);
            uint8_t ev_dci = (uint8_t)((ev.control >> 16) & 0x1fu);
            uint8_t ev_slot = (uint8_t)((ev.control >> 24) & 0xffu);

            if (type == XHCI_TRB_TYPE_TRANSFER_EVENT && ev_slot == slot_id && ev_dci == dci) {
                uint32_t code = xhci_completion_code(&ev);
                if (out_event) {
                    *out_event = ev;
                }
                if (out_code) {
                    *out_code = code;
                }
                return code == XHCI_COMPLETION_SUCCESS || code == XHCI_COMPLETION_SHORT_PACKET;
            }
        }
        asm volatile("pause");
    }

    log_error("xhci", "transfer timed out");
    return false;
}

static bool xhci_command(xhci_controller_t* hc,
                         uint32_t type,
                         uint64_t param,
                         uint32_t status,
                         uint32_t control_extra,
                         xhci_trb_t* out_event) {
    uint64_t cmd_phys = 0;
    uint8_t cycle = 0;
    uint32_t code = 0;
    xhci_trb_t* trb = xhci_ring_prepare(&hc->cmd_ring, &cmd_phys, &cycle);

    if (!trb) {
        return false;
    }

    trb->param_lo = (uint32_t)param;
    trb->param_hi = (uint32_t)(param >> 32);
    trb->status = status;
    trb->control = (type << 10) | control_extra | (cycle ? XHCI_TRB_CYCLE : 0u);
    xhci_ring_advance(&hc->cmd_ring);

    wr32(hc->db, 0u, 0u);
    if (!xhci_wait_command(hc, cmd_phys, out_event, &code)) {
        log_errorf("xhci", "command type=%u failed code=%u", type, code);
        return false;
    }

    return true;
}

static bool xhci_enable_slot(xhci_controller_t* hc, uint8_t* out_slot_id) {
    xhci_trb_t ev;

    if (!out_slot_id) {
        return false;
    }

    if (!xhci_command(hc, XHCI_TRB_TYPE_ENABLE_SLOT, 0u, 0u, 0u, &ev)) {
        return false;
    }

    *out_slot_id = (uint8_t)((ev.control >> 24) & 0xffu);
    return *out_slot_id != 0u;
}

static bool xhci_address_device(xhci_controller_t* hc, uint8_t slot_id, uint64_t input_ctx_phys) {
    return xhci_command(hc,
                        XHCI_TRB_TYPE_ADDRESS_DEVICE,
                        input_ctx_phys,
                        0u,
                        ((uint32_t)slot_id << 24),
                        NULL);
}

static bool xhci_disable_slot(xhci_controller_t* hc, uint8_t slot_id) {
    if (!hc || slot_id == 0u) {
        return true;
    }
    return xhci_command(hc,
                        XHCI_TRB_TYPE_DISABLE_SLOT,
                        0u,
                        0u,
                        ((uint32_t)slot_id << 24),
                        NULL);
}

static bool xhci_configure_endpoint(xhci_controller_t* hc, uint8_t slot_id, uint64_t input_ctx_phys) {
    return xhci_command(hc,
                        XHCI_TRB_TYPE_CONFIGURE_ENDPOINT,
                        input_ctx_phys,
                        0u,
                        ((uint32_t)slot_id << 24),
                        NULL);
}

static uint16_t xhci_ep0_packet_size(uint32_t speed) {
    switch (speed) {
        case 4:
        case 5:
            return 512u;
        case 2:
            return 8u;
        default:
            return 64u;
    }
}

static void xhci_fill_slot_ctx(xhci_controller_t* hc,
                               uint8_t* slot_ctx,
                               uint32_t speed,
                               uint8_t root_port,
                               uint8_t context_entries) {
    (void)hc;
    ctx_write32(slot_ctx, 0u, ((speed & 0x0fu) << 20) | ((uint32_t)context_entries << 27));
    ctx_write32(slot_ctx, 1u, ((uint32_t)root_port << 16));
}

static void xhci_fill_ep_ctx(uint8_t* ep_ctx,
                             uint8_t ep_type,
                             uint16_t max_packet,
                             xhci_ring_t* ring,
                             uint32_t avg_trb_len) {
    ctx_write32(ep_ctx, 0u, 0u);
    ctx_write32(ep_ctx, 1u, (3u << 1) | ((uint32_t)ep_type << 3) | ((uint32_t)max_packet << 16));
    ctx_write32(ep_ctx, 2u, (uint32_t)(ring->phys | ring->cycle));
    ctx_write32(ep_ctx, 3u, (uint32_t)(ring->phys >> 32));
    ctx_write32(ep_ctx, 4u, avg_trb_len);
}

static bool xhci_reset_port(xhci_controller_t* hc, uint32_t port) {
    uint32_t off = XHCI_OP_PORTS + ((port - 1u) * XHCI_PORT_STRIDE);
    uint32_t value = rd32(hc->op, off);

    if ((value & XHCI_PORTSC_CCS) == 0u) {
        return false;
    }

    wr32(hc->op, off, (value & ~XHCI_PORTSC_CHANGE_MASK) | XHCI_PORTSC_PR);
    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        value = rd32(hc->op, off);
        if ((value & XHCI_PORTSC_PR) == 0u && (value & XHCI_PORTSC_PED) != 0u) {
            xhci_clear_port_changes(hc, port);
            return true;
        }
        asm volatile("pause");
    }

    log_errorf("xhci", "port %u reset timed out portsc=%x", port, value);
    return false;
}

static uint64_t xhci_pack_setup(uint8_t request_type,
                                uint8_t request,
                                uint16_t value,
                                uint16_t index,
                                uint16_t len) {
    return (uint64_t)request_type |
           ((uint64_t)request << 8) |
           ((uint64_t)value << 16) |
           ((uint64_t)index << 32) |
           ((uint64_t)len << 48);
}

static bool xhci_ring_transfer(xhci_storage_ctx_t* dev,
                               xhci_ring_t* ring,
                               uint8_t dci,
                               uint64_t data_phys,
                               uint32_t len,
                               bool in) {
    xhci_controller_t* hc = dev->hc;
    uint64_t trb_phys = 0;
    uint8_t cycle = 0;
    uint32_t code = 0;
    xhci_trb_t* trb = xhci_ring_prepare(ring, &trb_phys, &cycle);

    (void)in;

    if (!trb || len > (PAGE_SIZE * 4u)) {
        return false;
    }

    trb->param_lo = (uint32_t)data_phys;
    trb->param_hi = (uint32_t)(data_phys >> 32);
    trb->status = len & 0x1ffffu;
    trb->control = (XHCI_TRB_TYPE_NORMAL << 10) |
                   XHCI_TRB_IOC |
                   (cycle ? XHCI_TRB_CYCLE : 0u);
    xhci_ring_advance(ring);

    wr32(hc->db, ((uint32_t)dev->slot_id * 4u), dci);
    if (!xhci_wait_transfer(hc, dev->slot_id, dci, NULL, &code)) {
        log_errorf("xhci", "bulk transfer failed slot=%u dci=%u code=%u",
                   dev->slot_id,
                   dci,
                   code);
        return false;
    }

    (void)trb_phys;
    return true;
}

static bool xhci_bulk_transfer(void* ctx,
                               uint8_t endpoint_addr,
                               bool in,
                               uint64_t data_phys,
                               uint32_t len,
                               uint16_t max_packet) {
    xhci_storage_ctx_t* dev = (xhci_storage_ctx_t*)ctx;
    uint8_t dci = 0;
    xhci_ring_t* ring = NULL;

    (void)max_packet;

    if (!dev || !dev->hc) {
        return false;
    }

    dci = in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ring = in ? &dev->bulk_in_ring : &dev->bulk_out_ring;
    if (dci == 0u || !ring->trbs || endpoint_addr == 0u) {
        return false;
    }

    return xhci_ring_transfer(dev, ring, dci, data_phys, len, in);
}

static bool xhci_control(xhci_storage_ctx_t* dev,
                         uint8_t request_type,
                         uint8_t request,
                         uint16_t value,
                         uint16_t index,
                         void* data,
                         uint16_t len) {
    xhci_controller_t* hc = NULL;
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    uint8_t* xfer_data = NULL;
    bool data_in = (request_type & 0x80u) != 0u;
    uint32_t code = 0;

    if (!dev || !dev->hc || len > 2048u) {
        return false;
    }

    hc = dev->hc;
    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    xfer_data = page + 1024u;
    if (!data_in && data && len != 0u) {
        memcpy(xfer_data, data, len);
    }

    {
        uint64_t trb_phys = 0;
        uint8_t cycle = 0;
        xhci_trb_t* trb = xhci_ring_prepare(&dev->ep0_ring, &trb_phys, &cycle);
        uint32_t trt = len == 0u ? 0u : (data_in ? 3u : 2u);

        if (!trb) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        trb->param_lo = (uint32_t)xhci_pack_setup(request_type, request, value, index, len);
        trb->param_hi = (uint32_t)(xhci_pack_setup(request_type, request, value, index, len) >> 32);
        trb->status = 8u;
        trb->control = (XHCI_TRB_TYPE_SETUP_STAGE << 10) |
                       XHCI_TRB_IDT |
                       (trt << 16) |
                       (cycle ? XHCI_TRB_CYCLE : 0u);
        xhci_ring_advance(&dev->ep0_ring);
        (void)trb_phys;
    }

    if (len != 0u) {
        uint64_t trb_phys = 0;
        uint8_t cycle = 0;
        xhci_trb_t* trb = xhci_ring_prepare(&dev->ep0_ring, &trb_phys, &cycle);

        if (!trb) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        trb->param_lo = (uint32_t)(page_phys + 1024u);
        trb->param_hi = (uint32_t)((page_phys + 1024u) >> 32);
        trb->status = len & 0x1ffffu;
        trb->control = (XHCI_TRB_TYPE_DATA_STAGE << 10) |
                       (data_in ? XHCI_TRB_DIR : 0u) |
                       (cycle ? XHCI_TRB_CYCLE : 0u);
        xhci_ring_advance(&dev->ep0_ring);
        (void)trb_phys;
    }

    {
        uint64_t trb_phys = 0;
        uint8_t cycle = 0;
        xhci_trb_t* trb = xhci_ring_prepare(&dev->ep0_ring, &trb_phys, &cycle);

        if (!trb) {
            pmm_free((void*)(uintptr_t)page_phys);
            return false;
        }

        trb->status = 0u;
        trb->control = (XHCI_TRB_TYPE_STATUS_STAGE << 10) |
                       XHCI_TRB_IOC |
                       ((!data_in) ? XHCI_TRB_DIR : 0u) |
                       (cycle ? XHCI_TRB_CYCLE : 0u);
        xhci_ring_advance(&dev->ep0_ring);
        (void)trb_phys;
    }

    wr32(hc->db, ((uint32_t)dev->slot_id * 4u), 1u);
    if (!xhci_wait_transfer(hc, dev->slot_id, 1u, NULL, &code)) {
        log_errorf("xhci", "control request %u failed code=%u", request, code);
        pmm_free((void*)(uintptr_t)page_phys);
        return false;
    }

    if (data_in && data && len != 0u) {
        memcpy(data, xfer_data, len);
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return true;
}

static bool parse_mass_storage_config(const uint8_t* cfg,
                                      uint16_t cfg_len,
                                      uint8_t* out_config_value,
                                      uint8_t* out_bulk_in,
                                      uint8_t* out_bulk_out,
                                      uint16_t* out_bulk_in_mps,
                                      uint16_t* out_bulk_out_mps) {
    bool in_mass_storage = false;
    uint16_t off = 0;

    if (!cfg || cfg_len < sizeof(usb_config_desc_t) ||
        !out_config_value || !out_bulk_in || !out_bulk_out ||
        !out_bulk_in_mps || !out_bulk_out_mps) {
        return false;
    }

    *out_config_value = ((const usb_config_desc_t*)cfg)->config_value;
    *out_bulk_in = 0u;
    *out_bulk_out = 0u;
    *out_bulk_in_mps = 0u;
    *out_bulk_out_mps = 0u;

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
        } else if (type == USB_DESC_ENDPOINT &&
                   in_mass_storage &&
                   len >= sizeof(usb_endpoint_desc_t)) {
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

static bool xhci_setup_addressed_device(xhci_controller_t* hc,
                                        xhci_storage_ctx_t* dev,
                                        uint32_t port,
                                        uint32_t speed,
                                        uint16_t ep0_mps) {
    uint64_t input_phys = 0;
    uint8_t* input = NULL;
    uint8_t* input_ctrl = NULL;
    uint8_t* slot_ctx = NULL;
    uint8_t* ep0_ctx = NULL;

    if (!dma_alloc_page(&input_phys, &input)) {
        return false;
    }

    input_ctrl = xhci_input_ctx_at(hc, input, 0u);
    slot_ctx = xhci_input_ctx_at(hc, input, 1u);
    ep0_ctx = xhci_input_ctx_at(hc, input, 2u);

    ctx_write32(input_ctrl, 1u, (1u << 0) | (1u << 1));
    xhci_fill_slot_ctx(hc, slot_ctx, speed, (uint8_t)port, 1u);
    xhci_fill_ep_ctx(ep0_ctx, 4u, ep0_mps, &dev->ep0_ring, 8u);

    hc->dcbaa[dev->slot_id] = dev->dev_ctx_phys;
    if (!xhci_address_device(hc, dev->slot_id, input_phys)) {
        pmm_free((void*)(uintptr_t)input_phys);
        return false;
    }

    pmm_free((void*)(uintptr_t)input_phys);
    return true;
}

static bool xhci_configure_bulk_endpoints(xhci_storage_ctx_t* dev,
                                          uint8_t bulk_in,
                                          uint8_t bulk_out,
                                          uint16_t bulk_in_mps,
                                          uint16_t bulk_out_mps) {
    xhci_controller_t* hc = dev->hc;
    uint64_t input_phys = 0;
    uint8_t* input = NULL;
    uint8_t* input_ctrl = NULL;
    uint8_t* slot_ctx = NULL;
    uint8_t* in_ctx = NULL;
    uint8_t* out_ctx = NULL;
    uint8_t max_dci = 0;

    dev->bulk_in_dci = xhci_dci_from_ep(bulk_in);
    dev->bulk_out_dci = xhci_dci_from_ep(bulk_out);
    max_dci = dev->bulk_in_dci > dev->bulk_out_dci ? dev->bulk_in_dci : dev->bulk_out_dci;

    if (!xhci_ring_alloc(&dev->bulk_in_ring, XHCI_RING_TRBS) ||
        !xhci_ring_alloc(&dev->bulk_out_ring, XHCI_RING_TRBS) ||
        !dma_alloc_page(&input_phys, &input)) {
        return false;
    }

    input_ctrl = xhci_input_ctx_at(hc, input, 0u);
    slot_ctx = xhci_input_ctx_at(hc, input, 1u);
    in_ctx = xhci_input_ctx_at(hc, input, (uint32_t)dev->bulk_in_dci + 1u);
    out_ctx = xhci_input_ctx_at(hc, input, (uint32_t)dev->bulk_out_dci + 1u);

    ctx_write32(input_ctrl,
                1u,
                (1u << 0) |
                (1u << dev->bulk_in_dci) |
                (1u << dev->bulk_out_dci));

    xhci_fill_slot_ctx(hc, slot_ctx, dev->speed, dev->port, max_dci);
    xhci_fill_ep_ctx(in_ctx, 6u, bulk_in_mps, &dev->bulk_in_ring, 512u);
    xhci_fill_ep_ctx(out_ctx, 2u, bulk_out_mps, &dev->bulk_out_ring, 512u);

    if (!xhci_configure_endpoint(hc, dev->slot_id, input_phys)) {
        pmm_free((void*)(uintptr_t)input_phys);
        return false;
    }

    pmm_free((void*)(uintptr_t)input_phys);
    return true;
}

static bool enumerate_storage_on_port(xhci_controller_t* hc,
                                      uint32_t port,
                                      uint32_t speed,
                                      uint32_t* out_storage_index) {
    xhci_storage_ctx_t* dev = NULL;
    usb_device_desc_t dev_desc;
    uint8_t cfg_first[9];
    uint8_t cfg_full[512];
    uint16_t cfg_len = 0;
    uint8_t config_value = 0;
    uint8_t bulk_in = 0;
    uint8_t bulk_out = 0;
    uint16_t bulk_in_mps = 0;
    uint16_t bulk_out_mps = 0;
    uint32_t out_index = 0;
    usb_bot_transport_t transport;
    uint8_t* dev_ctx_virt = NULL;
    uint16_t ep0_mps = 64u;

    if (!hc || g_xhci_storage_count >= XHCI_MAX_STORAGE_DEVS) {
        return false;
    }

    if (!xhci_reset_port(hc, port)) {
        return false;
    }

    {
        uint32_t off = XHCI_OP_PORTS + ((port - 1u) * XHCI_PORT_STRIDE);
        uint32_t post_reset = rd32(hc->op, off);
        uint32_t reset_speed = (post_reset >> 10) & 0x0fu;

        if (reset_speed != 0u && reset_speed != speed) {
            log_infof("xhci",
                      "port %u speed changed after reset: %s(%u) -> %s(%u)",
                      port,
                      xhci_speed_name(speed),
                      speed,
                      xhci_speed_name(reset_speed),
                      reset_speed);
            speed = reset_speed;
        } else if (speed == 0u) {
            speed = 3u;
            log_infof("xhci", "port %u speed unknown after reset; using high-speed fallback", port);
        }
    }

    ep0_mps = xhci_ep0_packet_size(speed);

    dev = &g_xhci_storage[g_xhci_storage_count];
    memset(dev, 0, sizeof(*dev));
    dev->hc = hc;
    dev->port = (uint8_t)port;
    dev->speed = (uint8_t)speed;

    if (!xhci_enable_slot(hc, &dev->slot_id)) {
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    if (!dma_alloc_page(&dev->dev_ctx_phys, &dev_ctx_virt) ||
        !xhci_ring_alloc(&dev->ep0_ring, XHCI_RING_TRBS)) {
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }
    dev->dev_ctx = dev_ctx_virt;

    if (!xhci_setup_addressed_device(hc, dev, port, speed, ep0_mps)) {
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!xhci_control(dev,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_DEVICE << 8),
                      0u,
                      &dev_desc,
                      sizeof(dev_desc))) {
        log_errorf("xhci", "port %u device descriptor failed", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    if (dev_desc.max_packet0 != 0u && dev_desc.max_packet0 != ep0_mps) {
        log_infof("xhci", "port %u ep0 mps reported=%u initial=%u",
                  port,
                  dev_desc.max_packet0,
                  ep0_mps);
    }

    memset(cfg_first, 0, sizeof(cfg_first));
    if (!xhci_control(dev,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_CONFIG << 8),
                      0u,
                      cfg_first,
                      sizeof(cfg_first))) {
        log_errorf("xhci", "port %u config header failed", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    cfg_len = ((const usb_config_desc_t*)cfg_first)->total_length;
    if (cfg_len < sizeof(usb_config_desc_t) || cfg_len > sizeof(cfg_full)) {
        log_errorf("xhci", "port %u config length unsupported: %u", port, cfg_len);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    memset(cfg_full, 0, sizeof(cfg_full));
    if (!xhci_control(dev,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_CONFIG << 8),
                      0u,
                      cfg_full,
                      cfg_len)) {
        log_errorf("xhci", "port %u full config failed", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    if (!parse_mass_storage_config(cfg_full,
                                   cfg_len,
                                   &config_value,
                                   &bulk_in,
                                   &bulk_out,
                                   &bulk_in_mps,
                                   &bulk_out_mps)) {
        log_infof("xhci", "port %u device is not SCSI/BOT mass storage", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    if (!xhci_control(dev, 0x00u, USB_REQ_SET_CONFIG, config_value, 0u, NULL, 0u)) {
        log_errorf("xhci", "port %u set-config failed", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    if (!xhci_configure_bulk_endpoints(dev, bulk_in, bulk_out, bulk_in_mps, bulk_out_mps)) {
        log_errorf("xhci", "port %u configure bulk endpoints failed", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    dev->present = true;
    dev->bulk_in_ep = bulk_in;
    dev->bulk_out_ep = bulk_out;
    dev->bulk_in_max_packet = bulk_in_mps;
    dev->bulk_out_max_packet = bulk_out_mps;

    memset(&transport, 0, sizeof(transport));
    transport.transport_name = "xHCI";
    transport.ctx = dev;
    transport.address = dev->slot_id;
    transport.bulk_in_ep = bulk_in;
    transport.bulk_out_ep = bulk_out;
    transport.bulk_in_max_packet = bulk_in_mps;
    transport.bulk_out_max_packet = bulk_out_mps;
    transport.bulk_transfer = xhci_bulk_transfer;

    if (!usb_storage_register_bot(&transport, &out_index)) {
        log_errorf("xhci", "port %u BOT registration failed", port);
        (void)xhci_disable_slot(hc, dev->slot_id);
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    if (out_storage_index) {
        *out_storage_index = out_index;
    }

    g_xhci_storage_count++;
    log_okf("xhci",
            "port %u registered USB storage slot=%u in=%x out=%x mps=%u/%u",
            port,
            dev->slot_id,
            bulk_in,
            bulk_out,
            bulk_in_mps,
            bulk_out_mps);
    return true;
}

static bool xhci_setup_controller(xhci_controller_t* hc) {
    uint32_t hcs1 = 0;
    uint32_t hcs2 = 0;
    uint32_t hcc1 = 0;
    uint32_t dboff = 0;
    uint32_t rtsoff = 0;
    uint8_t* dcbaa_virt = NULL;
    uint8_t* event_virt = NULL;
    uint8_t* erst_virt = NULL;

    hc->cap_len = rd8(hc->cap, XHCI_CAP_CAPLENGTH);
    hc->version = rd16(hc->cap, XHCI_CAP_HCIVERSION);
    hcs1 = rd32(hc->cap, XHCI_CAP_HCSPARAMS1);
    hcs2 = rd32(hc->cap, XHCI_CAP_HCSPARAMS2);
    hcc1 = rd32(hc->cap, XHCI_CAP_HCCPARAMS1);
    dboff = rd32(hc->cap, XHCI_CAP_DBOFF) & ~3u;
    rtsoff = rd32(hc->cap, XHCI_CAP_RTSOFF) & ~0x1fu;

    if (hc->cap_len == 0u || dboff == 0u || rtsoff == 0u) {
        log_error("xhci", "invalid capability registers");
        return false;
    }

    hc->op = hc->cap + hc->cap_len;
    hc->db = hc->cap + dboff;
    hc->rt = hc->cap + rtsoff;
    hc->max_slots = hcs1 & 0xffu;
    hc->max_ports = (hcs1 >> 24) & 0xffu;
    hc->ctx_size = (hcc1 & (1u << 2)) ? 64u : 32u;
    hc->scratchpad_count = ((hcs2 >> 27) & 0x1fu) | (((hcs2 >> 21) & 0x1fu) << 5);

    if (hc->max_ports > XHCI_MAX_PORTS) {
        hc->max_ports = XHCI_MAX_PORTS;
    }
    if (hc->max_slots > 64u) {
        hc->max_slots = 64u;
    }

    xhci_take_ownership(hc, hcc1);

    wr32(hc->op, XHCI_OP_USBCMD, rd32(hc->op, XHCI_OP_USBCMD) & ~XHCI_USBCMD_RS);
    if (!xhci_wait_status_set(hc, XHCI_USBSTS_HCH, true)) {
        log_error("xhci", "timeout waiting for halt");
        return false;
    }

    wr32(hc->op, XHCI_OP_USBCMD, rd32(hc->op, XHCI_OP_USBCMD) | XHCI_USBCMD_HCRST);
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if ((rd32(hc->op, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) == 0u) {
            break;
        }
        asm volatile("pause");
    }
    if (!xhci_wait_status_set(hc, XHCI_USBSTS_CNR, false)) {
        log_error("xhci", "timeout waiting for controller ready");
        return false;
    }

    if (!dma_alloc_page(&hc->dcbaa_phys, &dcbaa_virt) ||
        !xhci_ring_alloc(&hc->cmd_ring, XHCI_RING_TRBS) ||
        !dma_alloc_page(&hc->event_ring_phys, &event_virt) ||
        !dma_alloc_page(&hc->erst_phys, &erst_virt)) {
        log_error("xhci", "failed to allocate controller rings");
        return false;
    }

    hc->dcbaa = (uint64_t*)dcbaa_virt;
    hc->event_ring = (xhci_trb_t*)event_virt;
    hc->event_dequeue = 0u;
    hc->event_cycle = 1u;
    hc->erst = (xhci_erst_entry_t*)erst_virt;
    hc->erst[0].base = hc->event_ring_phys;
    hc->erst[0].size = XHCI_EVENT_TRBS;
    hc->erst[0].reserved = 0;

    if (hc->scratchpad_count > 0u) {
        uint8_t* scratch_array_virt = NULL;

        if (!dma_alloc_page(&hc->scratchpad_array_phys, &scratch_array_virt)) {
            return false;
        }

        hc->scratchpad_array = (uint64_t*)scratch_array_virt;
        for (uint32_t i = 0; i < hc->scratchpad_count && i < 512u; i++) {
            uint64_t scratch_phys = 0;
            uint8_t* scratch_virt = NULL;
            if (!dma_alloc_page(&scratch_phys, &scratch_virt)) {
                return false;
            }
            hc->scratchpad_array[i] = scratch_phys;
        }
        hc->dcbaa[0] = hc->scratchpad_array_phys;
    }

    wr64(hc->op, XHCI_OP_DCBAAP, hc->dcbaa_phys);
    wr64(hc->op, XHCI_OP_CRCR, hc->cmd_ring.phys | 1u);
    wr32(hc->rt + XHCI_INTR0_OFFSET, XHCI_INTR_ERSTSZ, 1u);
    wr64(hc->rt + XHCI_INTR0_OFFSET, XHCI_INTR_ERSTBA, hc->erst_phys);
    wr64(hc->rt + XHCI_INTR0_OFFSET, XHCI_INTR_ERDP, hc->event_ring_phys | (1ull << 3));
    wr32(hc->op, XHCI_OP_CONFIG, hc->max_slots);

    for (uint32_t i = 0; i < XHCI_MAX_PORTS; i++) {
        hc->port_state[i] = -1;
    }

    wr32(hc->op, XHCI_OP_USBCMD, rd32(hc->op, XHCI_OP_USBCMD) | XHCI_USBCMD_RS);
    if (!xhci_wait_status_set(hc, XHCI_USBSTS_HCH, false)) {
        log_error("xhci", "timeout starting controller");
        return false;
    }

    log_okf("xhci",
            "controller ready ver=%x slots=%u ports=%u ctx=%u scratch=%u",
            hc->version,
            hc->max_slots,
            hc->max_ports,
            hc->ctx_size,
            hc->scratchpad_count);
    return true;
}

void xhci_probe_mmio(uint8_t bus, uint8_t dev, uint8_t func, uint64_t mmio_phys) {
    xhci_controller_t* hc = NULL;

    if (mmio_phys == 0) {
        log_error("xhci", "invalid MMIO base");
        return;
    }

    for (uint32_t i = 0; i < g_xhci_count; i++) {
        if (g_xhci[i].present && g_xhci[i].mmio_phys == mmio_phys) {
            return;
        }
    }

    if (g_xhci_count >= XHCI_MAX_CONTROLLERS) {
        log_error("xhci", "controller limit reached");
        return;
    }

    hc = &g_xhci[g_xhci_count];
    memset(hc, 0, sizeof(*hc));
    hc->bus = bus;
    hc->dev = dev;
    hc->func = func;
    hc->mmio_phys = mmio_phys;
    hc->cap = xhci_map_window(mmio_phys);
    if (!hc->cap || !xhci_setup_controller(hc)) {
        log_error("xhci", "controller setup failed");
        memset(hc, 0, sizeof(*hc));
        return;
    }

    hc->present = true;
    g_xhci_count++;
}

static uint32_t xhci_storage_rescan_common(bool force) {
    uint32_t added = 0;

    for (uint32_t i = 0; i < g_xhci_count; i++) {
        xhci_controller_t* hc = &g_xhci[i];

        if (!hc->present) {
            continue;
        }

        for (uint32_t port = 1; port <= hc->max_ports; port++) {
            uint32_t off = XHCI_OP_PORTS + ((port - 1u) * XHCI_PORT_STRIDE);
            uint32_t portsc = rd32(hc->op, off);
            uint32_t speed = (portsc >> 10) & 0x0fu;
            bool changed = (portsc & XHCI_PORTSC_CHANGE_MASK) != 0u;
            int16_t state = hc->port_state[port - 1u];

            if ((portsc & XHCI_PORTSC_CCS) == 0u) {
                if (state >= 0) {
                    usb_storage_mark_removed((uint32_t)state);
                }
                hc->port_state[port - 1u] = -1;
                xhci_clear_port_changes(hc, port);
                continue;
            }

            if (state >= 0) {
                if (!usb_storage_is_present((uint32_t)state)) {
                    hc->port_state[port - 1u] = -1;
                } else {
                    continue;
                }
            } else if (state == -2 && !changed && !force) {
                continue;
            }

            log_infof("xhci",
                      "port %u connected speed=%s(%u) enabled=%u; enumerating",
                      port,
                      xhci_speed_name(speed),
                      speed,
                      (portsc & XHCI_PORTSC_PED) ? 1u : 0u);
            uint32_t storage_index = 0;
            if (enumerate_storage_on_port(hc, port, speed, &storage_index)) {
                hc->port_state[port - 1u] = (int16_t)storage_index;
                added++;
            } else {
                hc->port_state[port - 1u] = -2;
            }
            xhci_clear_port_changes(hc, port);
        }
    }

    return added;
}

uint32_t xhci_storage_rescan(void) {
    return xhci_storage_rescan_common(false);
}

uint32_t xhci_storage_rescan_force(void) {
    return xhci_storage_rescan_common(true);
}
