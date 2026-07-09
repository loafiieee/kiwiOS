#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/log.h"
#include "drivers/pci/pci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/usb_storage.h"
#include "libc/string.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#define EHCI_MAX_CONTROLLERS 4u
#define EHCI_MAX_PORTS 16u
#define EHCI_MAX_STORAGE_DEVS 8u
#define EHCI_DMA_LIMIT 0x100000000ull
#define EHCI_MAX_QTDS 16u

#define EHCI_PORT_EMPTY   (-1)
#define EHCI_PORT_IGNORED (-2)

#define EHCI_MMIO_VIRT_BASE 0xFFFFFFFFA2000000ull
#define EHCI_MMIO_SLOTS 4u
#define EHCI_MMIO_WINDOW_PAGES 8u

#define EHCI_CAP_CAPLENGTH  0x00u
#define EHCI_CAP_HCIVERSION 0x02u
#define EHCI_CAP_HCSPARAMS  0x04u
#define EHCI_CAP_HCCPARAMS  0x08u

#define EHCI_OP_USBCMD     0x00u
#define EHCI_OP_USBSTS     0x04u
#define EHCI_OP_USBINTR    0x08u
#define EHCI_OP_ASYNCLIST  0x18u
#define EHCI_OP_CONFIGFLAG 0x40u
#define EHCI_OP_PORTSC     0x44u

#define EHCI_USBCMD_RS      (1u << 0)
#define EHCI_USBCMD_HCRESET (1u << 1)
#define EHCI_USBCMD_PSE     (1u << 4)
#define EHCI_USBCMD_ASE     (1u << 5)

#define EHCI_USBSTS_HCH (1u << 12)

#define EHCI_PORTSC_CCS (1u << 0)
#define EHCI_PORTSC_CSC (1u << 1)
#define EHCI_PORTSC_PED (1u << 2)
#define EHCI_PORTSC_PEC (1u << 3)
#define EHCI_PORTSC_OCA (1u << 4)
#define EHCI_PORTSC_OCC (1u << 5)
#define EHCI_PORTSC_FPR (1u << 6)
#define EHCI_PORTSC_PR  (1u << 8)
#define EHCI_PORTSC_LS_MASK (3u << 10)
#define EHCI_PORTSC_PP  (1u << 12)
#define EHCI_PORTSC_PO  (1u << 13)
#define EHCI_PORTSC_WKCNNT_E (1u << 20)
#define EHCI_PORTSC_WKDSCNNT_E (1u << 21)
#define EHCI_PORTSC_WKOC_E (1u << 22)
#define EHCI_PORTSC_CHANGE_MASK (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | EHCI_PORTSC_OCC)

#define EHCI_PTR_TERM 1u
#define EHCI_PTR_QH   0x2u

#define EHCI_QTD_PID_OUT   0u
#define EHCI_QTD_PID_IN    1u
#define EHCI_QTD_PID_SETUP 2u

#define EHCI_QTD_ACTIVE (1u << 7)
#define EHCI_QTD_IOC    (1u << 15)
#define EHCI_QTD_ERR_MASK 0x7eu

#define EHCI_USBLEGSUP_CAP_ID 0x01u
#define EHCI_USBLEGSUP_BIOS_OWNED (1u << 16)
#define EHCI_USBLEGSUP_OS_OWNED   (1u << 24)

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_ADDRESS    5u
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

typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t ext_buffer[5];
} ehci_qtd_t;

typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t horiz_link;
    volatile uint32_t ep_char;
    volatile uint32_t ep_caps;
    volatile uint32_t current_qtd;
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t ext_buffer[5];
} ehci_qh_t;

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
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint64_t mmio_phys;

    volatile uint8_t* cap;
    volatile uint8_t* op;

    uint8_t cap_len;
    uint16_t version;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint32_t port_count;
    bool port_power_control;

    int16_t port_state[EHCI_MAX_PORTS];
    uint8_t ignored_line_state[EHCI_MAX_PORTS];
} ehci_controller_t;

typedef struct {
    bool present;
    ehci_controller_t* hc;
    uint8_t port;
    uint8_t address;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
} ehci_storage_ctx_t;

static uint64_t g_ehci_phys_pages[EHCI_MMIO_SLOTS];
static ehci_controller_t g_ehci[EHCI_MAX_CONTROLLERS];
static uint32_t g_ehci_count = 0;
static ehci_storage_ctx_t g_ehci_storage[EHCI_MAX_STORAGE_DEVS];
static uint32_t g_ehci_storage_count = 0;
static uint8_t g_next_ehci_address = 1;

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

static void tiny_delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) {
        asm volatile("pause");
    }
}

static bool dma_alloc_page(uint64_t* out_phys, uint8_t** out_virt) {
    void* phys = NULL;

    if (!out_phys || !out_virt) {
        return false;
    }

    phys = pmm_alloc_below(EHCI_DMA_LIMIT);
    if (!phys) {
        log_error("ehci", "failed to allocate 32-bit DMA page");
        return false;
    }

    *out_phys = (uint64_t)(uintptr_t)phys;
    *out_virt = (uint8_t*)hhdm_phys_to_virt(*out_phys);
    memset(*out_virt, 0, PAGE_SIZE);
    return true;
}

static volatile uint8_t* ehci_map_window(uint64_t mmio_phys) {
    page_table_t* kpt = vmm_get_kernel_page_table();
    uint64_t phys_page = PAGE_ALIGN_DOWN(mmio_phys);
    uint64_t off = mmio_phys - phys_page;

    for (uint32_t i = 0; i < EHCI_MMIO_SLOTS; i++) {
        if (g_ehci_phys_pages[i] == phys_page) {
            return (volatile uint8_t*)(EHCI_MMIO_VIRT_BASE +
                                       ((uint64_t)i * EHCI_MMIO_WINDOW_PAGES * PAGE_SIZE) +
                                       off);
        }
    }

    for (uint32_t i = 0; i < EHCI_MMIO_SLOTS; i++) {
        if (g_ehci_phys_pages[i] == 0) {
            uint64_t virt_base = EHCI_MMIO_VIRT_BASE +
                                 ((uint64_t)i * EHCI_MMIO_WINDOW_PAGES * PAGE_SIZE);

            for (uint32_t page = 0; page < EHCI_MMIO_WINDOW_PAGES; page++) {
                if (!vmm_map_page(kpt,
                                  virt_base + ((uint64_t)page * PAGE_SIZE),
                                  phys_page + ((uint64_t)page * PAGE_SIZE),
                                  PAGE_PRESENT | PAGE_WRITE)) {
                    log_error("ehci", "failed to map MMIO window");
                    return NULL;
                }
            }

            g_ehci_phys_pages[i] = phys_page;
            return (volatile uint8_t*)(virt_base + off);
        }
    }

    log_error("ehci", "MMIO map slots exhausted");
    return NULL;
}

static bool ehci_wait_halted(ehci_controller_t* hc, bool want_halted) {
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        bool halted = (rd32(hc->op, EHCI_OP_USBSTS) & EHCI_USBSTS_HCH) != 0u;
        if (halted == want_halted) {
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

static void ehci_take_ownership(ehci_controller_t* hc) {
    uint8_t cap_off = (uint8_t)((hc->hccparams >> 8) & 0xffu);

    for (uint32_t depth = 0; cap_off >= 0x40u && depth < 16u; depth++) {
        uint32_t cap = pci_config_read32(hc->bus, hc->dev, hc->func, cap_off);
        uint8_t cap_id = (uint8_t)(cap & 0xffu);
        uint8_t next = (uint8_t)((cap >> 8) & 0xffu);

        if (cap_id == EHCI_USBLEGSUP_CAP_ID) {
            if ((cap & EHCI_USBLEGSUP_BIOS_OWNED) != 0u) {
                pci_config_write32(hc->bus,
                                   hc->dev,
                                   hc->func,
                                   cap_off,
                                   cap | EHCI_USBLEGSUP_OS_OWNED);

                for (uint32_t spin = 0; spin < 1000000u; spin++) {
                    cap = pci_config_read32(hc->bus, hc->dev, hc->func, cap_off);
                    if ((cap & EHCI_USBLEGSUP_BIOS_OWNED) == 0u &&
                        (cap & EHCI_USBLEGSUP_OS_OWNED) != 0u) {
                        break;
                    }
                    asm volatile("pause");
                }
            }

            pci_config_write32(hc->bus, hc->dev, hc->func, (uint8_t)(cap_off + 4u), 0u);
            log_infof("ehci", "legacy ownership cap=%x value=%x", cap_off, cap);
            return;
        }

        cap_off = next;
    }
}

static void ehci_clear_port_changes(ehci_controller_t* hc, uint32_t port) {
    uint32_t off = EHCI_OP_PORTSC + ((port - 1u) * 4u);
    uint32_t value = rd32(hc->op, off);

    value &= ~(EHCI_PORTSC_PR |
               EHCI_PORTSC_FPR |
               EHCI_PORTSC_PO |
               EHCI_PORTSC_WKCNNT_E |
               EHCI_PORTSC_WKDSCNNT_E |
               EHCI_PORTSC_WKOC_E);
    wr32(hc->op, off, value | EHCI_PORTSC_CHANGE_MASK);
}

static void ehci_release_port_to_companion(ehci_controller_t* hc, uint32_t port) {
    uint32_t off = EHCI_OP_PORTSC + ((port - 1u) * 4u);
    uint32_t value = rd32(hc->op, off);

    value &= ~(EHCI_PORTSC_PR |
               EHCI_PORTSC_FPR |
               EHCI_PORTSC_WKCNNT_E |
               EHCI_PORTSC_WKDSCNNT_E |
               EHCI_PORTSC_WKOC_E);
    wr32(hc->op, off, value | EHCI_PORTSC_PO | EHCI_PORTSC_CHANGE_MASK);
    tiny_delay(100000u);
}

static void ehci_power_port(ehci_controller_t* hc, uint32_t port) {
    uint32_t off = EHCI_OP_PORTSC + ((port - 1u) * 4u);
    uint32_t value = rd32(hc->op, off);

    if (hc->port_power_control && (value & EHCI_PORTSC_PP) == 0u) {
        value &= ~(EHCI_PORTSC_CHANGE_MASK |
                   EHCI_PORTSC_PR |
                   EHCI_PORTSC_FPR |
                   EHCI_PORTSC_PO);
        wr32(hc->op, off, value | EHCI_PORTSC_PP);
        tiny_delay(100000u);
    }
}

static void ehci_reset_port(ehci_controller_t* hc, uint32_t port) {
    uint32_t off = EHCI_OP_PORTSC + ((port - 1u) * 4u);
    uint32_t value = rd32(hc->op, off);

    if ((value & EHCI_PORTSC_CCS) == 0u) {
        return;
    }

    value &= ~(EHCI_PORTSC_CHANGE_MASK |
               EHCI_PORTSC_FPR |
               EHCI_PORTSC_PO |
               EHCI_PORTSC_WKCNNT_E |
               EHCI_PORTSC_WKDSCNNT_E |
               EHCI_PORTSC_WKOC_E);
    wr32(hc->op, off, value | EHCI_PORTSC_PR);
    tiny_delay(500000u);

    value = rd32(hc->op, off);
    value &= ~(EHCI_PORTSC_PR |
               EHCI_PORTSC_CHANGE_MASK |
               EHCI_PORTSC_FPR |
               EHCI_PORTSC_PO |
               EHCI_PORTSC_WKCNNT_E |
               EHCI_PORTSC_WKDSCNNT_E |
               EHCI_PORTSC_WKOC_E);
    wr32(hc->op, off, value);
    tiny_delay(500000u);
    ehci_clear_port_changes(hc, port);
}

static void ehci_qtd_set_buffer(ehci_qtd_t* qtd, uint64_t phys) {
    uint64_t page = phys & ~0xfffull;

    for (uint32_t i = 0; i < 5u; i++) {
        uint64_t addr = page + ((uint64_t)i * PAGE_SIZE);
        qtd->buffer[i] = (uint32_t)(addr & 0xfffff000u);
        qtd->ext_buffer[i] = (uint32_t)(addr >> 32);
    }
    qtd->buffer[0] |= (uint32_t)(phys & 0xfffu);
}

static uint32_t ehci_qtd_token(uint32_t pid, uint32_t len, uint8_t data_toggle, bool ioc) {
    return ((uint32_t)(data_toggle & 1u) << 31) |
           ((len & 0x7fffu) << 16) |
           (ioc ? EHCI_QTD_IOC : 0u) |
           (3u << 10) |
           ((pid & 3u) << 8) |
           EHCI_QTD_ACTIVE;
}

static uint32_t ehci_qh_ep_char(uint8_t addr,
                                uint8_t endpoint,
                                uint16_t max_packet,
                                bool control_endpoint) {
    return ((uint32_t)(addr & 0x7fu)) |
           ((uint32_t)(endpoint & 0x0fu) << 8) |
           (2u << 12) |
           (1u << 14) |
           ((uint32_t)(max_packet & 0x7ffu) << 16) |
           (control_endpoint ? (1u << 27) : 0u) |
           (4u << 28);
}

static void ehci_qtd_init(ehci_qtd_t* qtd,
                          uint32_t next_phys,
                          uint32_t pid,
                          uint64_t data_phys,
                          uint32_t len,
                          uint8_t data_toggle,
                          bool ioc) {
    memset(qtd, 0, sizeof(*qtd));
    qtd->next = next_phys ? next_phys : EHCI_PTR_TERM;
    qtd->alt_next = EHCI_PTR_TERM;
    qtd->token = ehci_qtd_token(pid, len, data_toggle, ioc);
    if (len != 0u) {
        ehci_qtd_set_buffer(qtd, data_phys);
    }
}

static bool ehci_submit_qh(ehci_controller_t* hc,
                           uint8_t addr,
                           uint8_t endpoint,
                           uint16_t max_packet,
                           bool control_endpoint,
                           ehci_qtd_t* qtd,
                           uint32_t qtd_count,
                           uint32_t qtd_phys) {
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    ehci_qh_t* qh = NULL;
    uint32_t qh_phys = 0;
    bool done = false;
    bool ok = false;

    if (!hc || !qtd || qtd_count == 0u || qtd_count > EHCI_MAX_QTDS || max_packet == 0u) {
        return false;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    qh = (ehci_qh_t*)page;
    qh_phys = (uint32_t)page_phys;
    memset(qh, 0, sizeof(*qh));
    qh->horiz_link = qh_phys | EHCI_PTR_QH;
    qh->ep_char = ehci_qh_ep_char(addr, endpoint, max_packet, control_endpoint) | (1u << 15);
    qh->ep_caps = (1u << 30);
    qh->current_qtd = 0u;
    qh->next_qtd = qtd_phys;
    qh->alt_next_qtd = EHCI_PTR_TERM;
    qh->token = 0u;

    wr32(hc->op, EHCI_OP_ASYNCLIST, qh_phys);
    wr32(hc->op,
         EHCI_OP_USBCMD,
         (rd32(hc->op, EHCI_OP_USBCMD) | EHCI_USBCMD_RS | EHCI_USBCMD_ASE) & ~EHCI_USBCMD_PSE);

    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        done = true;
        for (uint32_t i = 0; i < qtd_count; i++) {
            if ((qtd[i].token & EHCI_QTD_ACTIVE) != 0u) {
                done = false;
                break;
            }
        }
        if (done) {
            break;
        }
        asm volatile("pause");
    }

    wr32(hc->op, EHCI_OP_USBCMD, rd32(hc->op, EHCI_OP_USBCMD) & ~EHCI_USBCMD_ASE);
    tiny_delay(10000u);
    wr32(hc->op, EHCI_OP_ASYNCLIST, 0u);

    if (!done) {
        log_error("ehci", "async transfer timed out");
        goto out;
    }

    ok = true;
    for (uint32_t i = 0; i < qtd_count; i++) {
        uint32_t status = qtd[i].token & 0xffu;
        if ((status & EHCI_QTD_ERR_MASK) != 0u) {
            log_errorf("ehci", "qTD error token=%x index=%u", qtd[i].token, i);
            ok = false;
            break;
        }
    }

out:
    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
}

static bool ehci_control(ehci_controller_t* hc,
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
    usb_setup_packet_t* setup = NULL;
    uint8_t* xfer_data = NULL;
    ehci_qtd_t* qtd = NULL;
    uint32_t qtd_phys = 0;
    uint32_t qtd_count = 0;
    bool is_in = (request_type & 0x80u) != 0u;
    bool ok = false;

    if (!hc || max_packet == 0u || len > 3072u) {
        return false;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    qtd = (ehci_qtd_t*)page;
    setup = (usb_setup_packet_t*)(page + 512u);
    xfer_data = page + 1024u;
    qtd_phys = (uint32_t)page_phys;

    setup->bm_request_type = request_type;
    setup->b_request = request;
    setup->w_value = value;
    setup->w_index = index;
    setup->w_length = len;

    if (!is_in && data && len != 0u) {
        memcpy(xfer_data, data, len);
    }

    ehci_qtd_init(&qtd[qtd_count],
                  (uint32_t)(qtd_phys + ((qtd_count + 1u) * sizeof(ehci_qtd_t))),
                  EHCI_QTD_PID_SETUP,
                  page_phys + 512u,
                  sizeof(*setup),
                  0u,
                  false);
    qtd_count++;

    if (len != 0u) {
        ehci_qtd_init(&qtd[qtd_count],
                      (uint32_t)(qtd_phys + ((qtd_count + 1u) * sizeof(ehci_qtd_t))),
                      is_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                      page_phys + 1024u,
                      len,
                      1u,
                      false);
        qtd_count++;
    }

    ehci_qtd_init(&qtd[qtd_count],
                  0u,
                  is_in ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN,
                  0u,
                  0u,
                  1u,
                  true);
    qtd_count++;

    if (qtd_count > 1u) {
        qtd[qtd_count - 2u].next = (uint32_t)(qtd_phys + ((qtd_count - 1u) * sizeof(ehci_qtd_t)));
    }

    ok = ehci_submit_qh(hc, addr, 0u, max_packet, true, qtd, qtd_count, qtd_phys);
    if (ok && is_in && data && len != 0u) {
        memcpy(data, xfer_data, len);
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
}

static uint8_t ehci_next_toggle(uint8_t toggle, uint32_t len, uint16_t max_packet) {
    uint32_t packets = 0;

    if (max_packet == 0u) {
        return toggle;
    }

    packets = (len == 0u) ? 1u : ((len + max_packet - 1u) / max_packet);
    if ((packets & 1u) != 0u) {
        toggle ^= 1u;
    }
    return toggle;
}

static bool ehci_bulk_transfer(void* ctx,
                               uint8_t endpoint_addr,
                               bool in,
                               uint64_t data_phys,
                               uint32_t len,
                               uint16_t max_packet) {
    ehci_storage_ctx_t* dev = (ehci_storage_ctx_t*)ctx;
    uint64_t page_phys = 0;
    uint8_t* page = NULL;
    ehci_qtd_t* qtd = NULL;
    uint32_t qtd_phys = 0;
    uint8_t* toggle = NULL;
    bool ok = false;

    if (!dev || !dev->hc || max_packet == 0u || len > (PAGE_SIZE * 4u)) {
        return false;
    }

    if (!dma_alloc_page(&page_phys, &page)) {
        return false;
    }

    qtd = (ehci_qtd_t*)page;
    qtd_phys = (uint32_t)page_phys;
    toggle = in ? &dev->bulk_in_toggle : &dev->bulk_out_toggle;

    ehci_qtd_init(qtd,
                  0u,
                  in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                  data_phys,
                  len,
                  *toggle,
                  true);

    ok = ehci_submit_qh(dev->hc,
                        dev->address,
                        endpoint_addr & 0x0fu,
                        max_packet,
                        false,
                        qtd,
                        1u,
                        qtd_phys);

    if (ok) {
        *toggle = ehci_next_toggle(*toggle, len, max_packet);
    }

    pmm_free((void*)(uintptr_t)page_phys);
    return ok;
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

static bool enumerate_storage_on_port(ehci_controller_t* hc, uint8_t port, uint32_t* out_storage_index) {
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
    uint8_t max_packet0 = 64u;
    ehci_storage_ctx_t* ctx = NULL;
    usb_bot_transport_t transport;
    uint32_t out_index = 0;
    uint32_t portsc = 0;

    if (!hc || g_ehci_storage_count >= EHCI_MAX_STORAGE_DEVS) {
        return false;
    }

    ehci_power_port(hc, port);
    ehci_reset_port(hc, port);

    portsc = rd32(hc->op, EHCI_OP_PORTSC + ((uint32_t)(port - 1u) * 4u));
    if ((portsc & EHCI_PORTSC_CCS) == 0u || (portsc & EHCI_PORTSC_PED) == 0u) {
        log_infof("ehci",
                  "port %u is not a high-speed enabled device after reset portsc=%x",
                  port,
                  portsc);
        return false;
    }

    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!ehci_control(hc,
                      0u,
                      64u,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_DEVICE << 8),
                      0u,
                      &dev_desc,
                      8u)) {
        log_errorf("ehci", "port %u failed first device descriptor", port);
        return false;
    }

    max_packet0 = dev_desc.max_packet0 ? dev_desc.max_packet0 : 64u;
    address = g_next_ehci_address++;
    if (g_next_ehci_address >= 120u) {
        g_next_ehci_address = 1u;
    }

    if (!ehci_control(hc, 0u, max_packet0, 0x00u, USB_REQ_SET_ADDRESS, address, 0u, NULL, 0u)) {
        log_errorf("ehci", "port %u set-address failed", port);
        return false;
    }
    tiny_delay(200000u);

    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!ehci_control(hc,
                      address,
                      max_packet0,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_DEVICE << 8),
                      0u,
                      &dev_desc,
                      sizeof(dev_desc))) {
        log_errorf("ehci", "port %u full device descriptor failed", port);
        return false;
    }

    memset(cfg_first, 0, sizeof(cfg_first));
    if (!ehci_control(hc,
                      address,
                      max_packet0,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_CONFIG << 8),
                      0u,
                      cfg_first,
                      sizeof(cfg_first))) {
        log_errorf("ehci", "port %u config header failed", port);
        return false;
    }

    cfg_len = ((const usb_config_desc_t*)cfg_first)->total_length;
    if (cfg_len < sizeof(usb_config_desc_t) || cfg_len > sizeof(cfg_full)) {
        log_errorf("ehci", "port %u config length unsupported: %u", port, cfg_len);
        return false;
    }

    memset(cfg_full, 0, sizeof(cfg_full));
    if (!ehci_control(hc,
                      address,
                      max_packet0,
                      0x80u,
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(USB_DESC_CONFIG << 8),
                      0u,
                      cfg_full,
                      cfg_len)) {
        log_errorf("ehci", "port %u full config failed", port);
        return false;
    }

    if (!parse_mass_storage_config(cfg_full,
                                   cfg_len,
                                   &config_value,
                                   &bulk_in,
                                   &bulk_out,
                                   &bulk_in_mps,
                                   &bulk_out_mps)) {
        log_infof("ehci", "port %u device is not SCSI/BOT mass storage", port);
        return false;
    }

    if (!ehci_control(hc, address, max_packet0, 0x00u, USB_REQ_SET_CONFIG, config_value, 0u, NULL, 0u)) {
        log_errorf("ehci", "port %u set-config failed", port);
        return false;
    }

    ctx = &g_ehci_storage[g_ehci_storage_count];
    memset(ctx, 0, sizeof(*ctx));
    ctx->present = true;
    ctx->hc = hc;
    ctx->port = port;
    ctx->address = address;
    ctx->bulk_in_ep = bulk_in;
    ctx->bulk_out_ep = bulk_out;
    ctx->bulk_in_max_packet = bulk_in_mps;
    ctx->bulk_out_max_packet = bulk_out_mps;

    memset(&transport, 0, sizeof(transport));
    transport.transport_name = "EHCI";
    transport.ctx = ctx;
    transport.address = address;
    transport.bulk_in_ep = bulk_in;
    transport.bulk_out_ep = bulk_out;
    transport.bulk_in_max_packet = bulk_in_mps;
    transport.bulk_out_max_packet = bulk_out_mps;
    transport.bulk_transfer = ehci_bulk_transfer;

    if (!usb_storage_register_bot(&transport, &out_index)) {
        memset(ctx, 0, sizeof(*ctx));
        log_errorf("ehci", "port %u BOT registration failed", port);
        return false;
    }

    if (out_storage_index) {
        *out_storage_index = out_index;
    }

    g_ehci_storage_count++;
    log_okf("ehci",
            "port %u registered USB storage addr=%u in=%x out=%x mps=%u/%u",
            port,
            address,
            bulk_in,
            bulk_out,
            bulk_in_mps,
            bulk_out_mps);
    return true;
}

static bool ehci_setup_controller(ehci_controller_t* hc) {
    hc->cap_len = rd8(hc->cap, EHCI_CAP_CAPLENGTH);
    hc->version = rd16(hc->cap, EHCI_CAP_HCIVERSION);
    hc->hcsparams = rd32(hc->cap, EHCI_CAP_HCSPARAMS);
    hc->hccparams = rd32(hc->cap, EHCI_CAP_HCCPARAMS);
    hc->port_count = hc->hcsparams & 0x0fu;
    hc->port_power_control = (hc->hcsparams & (1u << 4)) != 0u;

    if (hc->cap_len == 0u || hc->port_count == 0u) {
        log_error("ehci", "invalid capability registers");
        return false;
    }
    if (hc->port_count > EHCI_MAX_PORTS) {
        hc->port_count = EHCI_MAX_PORTS;
    }

    hc->op = hc->cap + hc->cap_len;
    ehci_take_ownership(hc);

    wr32(hc->op,
         EHCI_OP_USBCMD,
         rd32(hc->op, EHCI_OP_USBCMD) & ~(EHCI_USBCMD_RS | EHCI_USBCMD_ASE | EHCI_USBCMD_PSE));
    if (!ehci_wait_halted(hc, true)) {
        log_error("ehci", "timeout waiting for halt");
        return false;
    }

    wr32(hc->op, EHCI_OP_USBINTR, 0u);
    wr32(hc->op, EHCI_OP_USBCMD, rd32(hc->op, EHCI_OP_USBCMD) | EHCI_USBCMD_HCRESET);
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if ((rd32(hc->op, EHCI_OP_USBCMD) & EHCI_USBCMD_HCRESET) == 0u) {
            break;
        }
        asm volatile("pause");
    }

    wr32(hc->op, EHCI_OP_ASYNCLIST, 0u);
    wr32(hc->op, EHCI_OP_CONFIGFLAG, 1u);

    for (uint32_t port = 1; port <= hc->port_count; port++) {
        hc->port_state[port - 1u] = EHCI_PORT_EMPTY;
        hc->ignored_line_state[port - 1u] = 0xffu;
        ehci_power_port(hc, port);
        ehci_clear_port_changes(hc, port);
    }

    wr32(hc->op, EHCI_OP_USBCMD, EHCI_USBCMD_RS);
    if (!ehci_wait_halted(hc, false)) {
        log_error("ehci", "timeout starting controller");
        return false;
    }

    log_okf("ehci",
            "controller ready ver=%x ports=%u ppc=%u hcs=%x hcc=%x",
            hc->version,
            hc->port_count,
            hc->port_power_control ? 1u : 0u,
            hc->hcsparams,
            hc->hccparams);
    return true;
}

void ehci_probe_mmio(uint8_t bus, uint8_t dev, uint8_t func, uint64_t mmio_phys) {
    ehci_controller_t* hc = NULL;

    if (mmio_phys == 0) {
        log_error("ehci", "invalid MMIO base");
        return;
    }

    for (uint32_t i = 0; i < g_ehci_count; i++) {
        if (g_ehci[i].present && g_ehci[i].mmio_phys == mmio_phys) {
            return;
        }
    }

    if (g_ehci_count >= EHCI_MAX_CONTROLLERS) {
        log_error("ehci", "controller limit reached");
        return;
    }

    hc = &g_ehci[g_ehci_count];
    memset(hc, 0, sizeof(*hc));
    hc->bus = bus;
    hc->dev = dev;
    hc->func = func;
    hc->mmio_phys = mmio_phys;
    hc->cap = ehci_map_window(mmio_phys);
    if (!hc->cap || !ehci_setup_controller(hc)) {
        log_error("ehci", "controller setup failed");
        memset(hc, 0, sizeof(*hc));
        return;
    }

    hc->present = true;
    g_ehci_count++;
}

static uint32_t ehci_storage_rescan_common(bool force) {
    uint32_t added = 0;

    for (uint32_t i = 0; i < g_ehci_count; i++) {
        ehci_controller_t* hc = &g_ehci[i];

        if (!hc->present) {
            continue;
        }

        for (uint32_t port = 1; port <= hc->port_count; port++) {
            uint32_t port_index = port - 1u;
            uint32_t off = EHCI_OP_PORTSC + ((port - 1u) * 4u);
            uint32_t portsc = rd32(hc->op, off);
            bool changed = (portsc & EHCI_PORTSC_CHANGE_MASK) != 0u;
            int16_t state = hc->port_state[port_index];

            if ((portsc & EHCI_PORTSC_CCS) == 0u) {
                if (state >= 0) {
                    usb_storage_mark_removed((uint32_t)state);
                }
                hc->port_state[port_index] = EHCI_PORT_EMPTY;
                hc->ignored_line_state[port_index] = 0xffu;
                ehci_clear_port_changes(hc, port);
                continue;
            }

            if (state >= 0) {
                if (!usb_storage_is_present((uint32_t)state)) {
                    hc->port_state[port_index] = EHCI_PORT_EMPTY;
                } else {
                    continue;
                }
            } else if (state == EHCI_PORT_IGNORED && !force) {
                if (changed) {
                    ehci_clear_port_changes(hc, port);
                }
                continue;
            }

            if ((portsc & EHCI_PORTSC_LS_MASK) != 0u) {
                uint8_t line_state = (uint8_t)((portsc & EHCI_PORTSC_LS_MASK) >> 10);

                if (state == EHCI_PORT_IGNORED &&
                    hc->ignored_line_state[port_index] == line_state) {
                    if (changed) {
                        ehci_clear_port_changes(hc, port);
                    }
                    continue;
                }

                log_infof("ehci",
                          "port %u connected at non-high-speed line state portsc=%x; releasing to companion controller",
                          port,
                          portsc);
                hc->port_state[port_index] = EHCI_PORT_IGNORED;
                hc->ignored_line_state[port_index] = line_state;
                ehci_release_port_to_companion(hc, port);
                continue;
            }

            uint32_t storage_index = 0;
            if (enumerate_storage_on_port(hc, (uint8_t)port, &storage_index)) {
                hc->port_state[port_index] = (int16_t)storage_index;
                hc->ignored_line_state[port_index] = 0xffu;
                added++;
            } else {
                portsc = rd32(hc->op, off);
                log_infof("ehci",
                          "port %u connected enabled=%u owned=%u overcurrent=%u; no storage registered",
                          port,
                          (portsc & EHCI_PORTSC_PED) ? 1u : 0u,
                          (portsc & EHCI_PORTSC_PO) ? 1u : 0u,
                          (portsc & EHCI_PORTSC_OCA) ? 1u : 0u);
                hc->port_state[port_index] = EHCI_PORT_IGNORED;
                hc->ignored_line_state[port_index] = 0xffu;
            }

            ehci_clear_port_changes(hc, port);
        }
    }

    return added;
}

uint32_t ehci_storage_rescan(void) {
    return ehci_storage_rescan_common(false);
}

uint32_t ehci_storage_rescan_force(void) {
    return ehci_storage_rescan_common(true);
}
