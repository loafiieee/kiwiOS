#include <stdint.h>
#include <stddef.h>
#include "arch/x86/io.h"
#include "core/log.h"
#include "drivers/ahci/ahci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/usb_storage.h"
#include "drivers/usb/xhci.h"

// PCI legacy config I/O ports
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline uint32_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    return (uint32_t)(0x80000000u
        | ((uint32_t)bus  << 16)
        | ((uint32_t)dev  << 11)
        | ((uint32_t)func << 8)
        | (off & 0xFC));
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, func, off);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, value);
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t value) {
    // Read-modify-write the aligned 32-bit register, then write back.
    uint32_t orig = pci_read32(bus, dev, func, off);
    uint32_t shift = (uint32_t)((off & 2) * 8);
    uint32_t mask = (uint32_t)(0xFFFFu << shift);
    uint32_t next = (orig & ~mask) | ((uint32_t)value << shift);
    pci_write32(bus, dev, func, off, next);
}

static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, func, off);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

// Public: read BARn (32-bit read). BAR0..BAR5
uint32_t pci_read_bar32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    return pci_read32(bus, dev, func, (uint8_t)(0x10 + bar_index * 4));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    return pci_read32(bus, dev, func, off);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t value) {
    pci_write32(bus, dev, func, off, value);
}

static const char* pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    // Minimal, just enough to recognize what you care about early
    // AHCI is class 0x01, subclass 0x06, prog_if 0x01
    if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) return "SATA (AHCI)";
    if (class_code == 0x0C && subclass == 0x03) {
        if (prog_if == 0x00) return "USB controller (UHCI)";
        if (prog_if == 0x10) return "USB controller (OHCI)";
        if (prog_if == 0x20) return "USB controller (EHCI)";
        if (prog_if == 0x30) return "USB controller (xHCI)";
        return "USB controller";
    }
    if (class_code == 0x02) return "Network controller";
    if (class_code == 0x03) return "Display controller";
    return "Other";
}

void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    // PCI command register at offset 0x04
    // bit 2 = Bus Master Enable
    // bit 1 = Memory Space Enable (commonly needed for MMIO BARs)
    uint16_t cmd = pci_read16(bus, dev, func, 0x04);
    uint16_t next = (uint16_t)(cmd | (1u << 2) | (1u << 1));
    if (next != cmd) {
        pci_write16(bus, dev, func, 0x04, next);
        log_infof("pci", "Enabled bus mastering: %x:%x.%u CMD %x -> %x",
                  (unsigned)bus, (unsigned)dev, (unsigned)func, cmd, next);
    }
}

void pci_enable_io_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_read16(bus, dev, func, 0x04);
    uint16_t next = (uint16_t)(cmd | (1u << 0) | (1u << 2));
    if (next != cmd) {
        pci_write16(bus, dev, func, 0x04, next);
        log_infof("pci", "Enabled I/O bus mastering: %x:%x.%u CMD %x -> %x",
                  (unsigned)bus, (unsigned)dev, (unsigned)func, cmd, next);
    }
}

void pci_enumerate_and_log(void) {
    log_info("pci", "Scanning buses 0..255");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_read16((uint8_t)bus, dev, func, 0x00);
                if (vendor == 0xFFFF) {
                    if (func == 0) break; // no device at func0 => no funcs
                    continue;
                }

                uint16_t device = pci_read16((uint8_t)bus, dev, func, 0x02);
                uint8_t class_code = pci_read8((uint8_t)bus, dev, func, 0x0B);
                uint8_t subclass   = pci_read8((uint8_t)bus, dev, func, 0x0A);
                uint8_t prog_if    = pci_read8((uint8_t)bus, dev, func, 0x09);

                const char* cname = pci_class_name(class_code, subclass, prog_if);

                log_infof("pci",
                          "PCI %x:%x.%u vendor=%x device=%x class=%x:%x progIF=%x (%s)",
                          (unsigned)bus, (unsigned)dev, (unsigned)func,
                          vendor, device, class_code, subclass, prog_if, cname);

                // If this is an AHCI controller, print BAR5 (AHCI MMIO base)
                if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    // AHCI needs DMA, so ensure PCI bus mastering is enabled.
                    pci_enable_bus_master((uint8_t)bus, dev, func);

                    uint32_t bar5 = pci_read_bar32((uint8_t)bus, dev, func, 5);

                    // For MMIO BARs, low bits are flags. Mask them off.
                    // (AHCI BAR5 is typically a 32-bit MMIO BAR in QEMU)
                    uint32_t mmio = bar5 & ~0x0Fu;

                    log_infof("ahci",
                              "AHCI at %x:%x.%u BAR5=%x mmio=%x",
                              (unsigned)bus, (unsigned)dev, (unsigned)func,
                              bar5, mmio);
                    ahci_probe_mmio(mmio);

                }

                if (class_code == 0x0C && subclass == 0x03) {
                    if (prog_if == 0x00) {
                        uint32_t bar4 = pci_read_bar32((uint8_t)bus, dev, func, 4);
                        uint16_t io_base = (uint16_t)(bar4 & ~0x1fu);

                        usb_note_controller(USB_CONTROLLER_UHCI,
                                            (uint8_t)bus,
                                            dev,
                                            func,
                                            io_base,
                                            false,
                                            true);

                        if ((bar4 & 1u) == 0u || io_base == 0u) {
                            log_errorf("usb", "UHCI BAR4 is not a usable I/O BAR: %x", bar4);
                            continue;
                        }

                        pci_enable_io_bus_master((uint8_t)bus, dev, func);
                        log_infof("usb",
                                  "UHCI at %x:%x.%u BAR4=%x io=%x",
                                  (unsigned)bus, (unsigned)dev, (unsigned)func,
                                  bar4, io_base);
                        uhci_probe_pci((uint8_t)bus, dev, func, io_base);
                    } else if (prog_if == 0x10) {
                        uint32_t bar0 = pci_read_bar32((uint8_t)bus, dev, func, 0);
                        uint32_t mmio = bar0 & ~0x0fu;
                        usb_note_controller(USB_CONTROLLER_OHCI,
                                            (uint8_t)bus,
                                            dev,
                                            func,
                                            mmio,
                                            true,
                                            false);
                        log_errorf("usb",
                                   "OHCI controller at %x:%x.%u is detected but not supported yet",
                                   (unsigned)bus,
                                   (unsigned)dev,
                                   (unsigned)func);
                    } else if (prog_if == 0x20) {
                        uint32_t bar0 = pci_read_bar32((uint8_t)bus, dev, func, 0);
                        uint32_t mmio = bar0 & ~0x0fu;
                        usb_note_controller(USB_CONTROLLER_EHCI,
                                            (uint8_t)bus,
                                            dev,
                                            func,
                                            mmio,
                                            true,
                                            true);
                        pci_enable_bus_master((uint8_t)bus, dev, func);
                        log_infof("usb",
                                  "EHCI at %x:%x.%u BAR0=%x mmio=%x",
                                  (unsigned)bus,
                                  (unsigned)dev,
                                  (unsigned)func,
                                  bar0,
                                  mmio);
                        ehci_probe_mmio((uint8_t)bus, dev, func, mmio);
                    } else if (prog_if == 0x30) {
                        uint32_t bar0 = pci_read_bar32((uint8_t)bus, dev, func, 0);
                        uint32_t bar1 = pci_read_bar32((uint8_t)bus, dev, func, 1);
                        uint64_t mmio = ((bar0 & 0x6u) == 0x4u)
                                            ? (((uint64_t)bar1 << 32) | (uint64_t)(bar0 & ~0x0fu))
                                            : (uint64_t)(bar0 & ~0x0fu);
                        usb_note_controller(USB_CONTROLLER_XHCI,
                                            (uint8_t)bus,
                                            dev,
                                            func,
                                            mmio,
                                            true,
                                            true);
                        pci_enable_bus_master((uint8_t)bus, dev, func);
                        log_infof("usb",
                                  "xHCI at %x:%x.%u BAR0=%x mmio=%x",
                                  (unsigned)bus,
                                  (unsigned)dev,
                                  (unsigned)func,
                                  bar0,
                                  (uint32_t)mmio);
                        xhci_probe_mmio((uint8_t)bus, dev, func, mmio);
                    } else {
                        usb_note_controller(USB_CONTROLLER_UNKNOWN,
                                            (uint8_t)bus,
                                            dev,
                                            func,
                                            0,
                                            true,
                                            false);
                        log_errorf("usb",
                                   "USB controller at %x:%x.%u has unsupported progIF=%x",
                                   (unsigned)bus,
                                   (unsigned)dev,
                                   (unsigned)func,
                                   prog_if);
                    }
                }
            }
        }
    }

    log_info("pci", "PCI scan complete");
}
