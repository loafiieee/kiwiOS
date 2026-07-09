#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/hpet.h"
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "arch/x86/rtc.h"
#include "arch/x86/apic.h"
#include "arch/x86/syscall.h"
#include "arch/x86/tss.h"
#include "core/boot.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/kxe.h"
#include "core/log.h"
#include "core/scheduler.h"
#include "core/shell.h"
#include "core/syscall.h"
#include "core/time.h"
#include "libc/string.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "drivers/pci/pci.h"
#include "drivers/acpi/acpi.h"
#include "drivers/serial/serial.h"
#include "drivers/block/block.h"
#include "fs/bcache.h"
#include "fs/devfs/devfs.h"
#include "vfs/vfs.h"

static void init_pic(void) {
    // Initialize PIC (Programmable Interrupt Controller)
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // Mask all interrupts initially
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    // Unmask IRQ0 (timer) and IRQ1 (keyboard)
    outb(0x21, 0xFC);
}

static void init_pit(uint32_t hz) {
    uint16_t divisor = 0;

    if (hz == 0) {
        hz = 100;
    }

    divisor = (uint16_t)(1193180u / hz);
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFFu));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFFu));
}

// Enable x86_64 FPU/SSE for both kernel and userspace.
static void x86_enable_sse(void) {
    uint64_t cr0, cr4;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));

    // CR0: clear EM (bit 2) to enable FPU, set MP (bit 1) for proper WAIT/FWAIT.
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);

    // CR4: enable OS support for FXSAVE/FXRSTOR and SIMD exception handling.
    cr4 |= (1ULL << 9);   // OSFXSR
    cr4 |= (1ULL << 10);  // OSXMMEXCPT

    asm volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    asm volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    // Initialize FPU state.
    asm volatile ("fninit");
}

void kmain(void) {
    struct limine_framebuffer* fb = NULL;

    if (!boot_limine_supported()) {
        boot_hcf();
    }

    struct limine_hhdm_response *hhdm = boot_hhdm_response();
    if (!hhdm || hhdm->offset == 0) {
        boot_hcf();
    }
    hhdm_set_offset(hhdm->offset);    

    console_init();
    console_clear();
    fb = console_primary_framebuffer();
    log_ok("console", "Framebuffer console initialized");

    bool serial_ok = serial_init();
    if (serial_ok) {
        log_ok("serial", "COM1 initialized");
    } else {
        log_error("serial", "COM1 init failed (still may print on some setups)");
    }

    log_enable_serial(serial_ok);
    idt_enable_serial(serial_ok);

    // Disable interrupts during initialization
    asm volatile ("cli");

    init_idt();
    log_ok("interrupts", "IDT installed");

    tss_init();
    gdt_init();
    log_ok("cpu", "GDT/TSS configured");

    syscall_init();
    log_ok("cpu", "SYSCALL/SYSRET configured");

    x86_enable_sse();
    log_ok("cpu", "SSE enabled");

    struct limine_memmap_response *memmap = boot_memmap_response();
    if (memmap) {
        pmm_init(memmap);
        log_ok("memory", "Physical memory manager ready");
    } else {
        log_error("memory", "No Limine memory map provided");
        boot_hcf();
    }

    vmm_init();
    heap_init();
    log_ok("memory", "Virtual memory and heap initialized");

    acpi_init(boot_rsdp_response());
    apic_probe_from_acpi();
    hpet_probe_from_acpi();

    time_init(KIWI_TIMER_DEFAULT_HZ);
    if (hpet_timekeeping_start()) {
        time_set_highres_reader(hpet_monotonic_ns);
        log_ok("time", "HPET selected as monotonic read source");
    }
    {
        const acpi_fadt_info_t* fadt = acpi_fadt_info();
        uint8_t century_register = (fadt && fadt->present) ? fadt->century_register : 0u;
        uint64_t rtc_seconds = 0;

        if (rtc_read_unix_time_with_century(century_register, &rtc_seconds)) {
            time_set_realtime_unix(rtc_seconds);
            log_okf("time", "RTC wall clock initialized (century_reg=%x)", century_register ? century_register : 0x32u);
        } else {
            log_info("time", "RTC wall clock unavailable; CLOCK_REALTIME starts at zero");
        }
    }
    init_pic();
    init_pit(KIWI_TIMER_DEFAULT_HZ);
    log_info("interrupts", "PIC initialized and timer/keyboard unmasked");

    // Enable interrupts
    asm volatile ("sti");
    log_info("kernel", "Interrupts enabled");

    pci_enumerate_and_log();
    log_ok("pci", "PCI enumeration complete");

    block_init();
    log_ok("block", "Block devices initialized");
    bcache_init(256);
    log_ok("bcache", "Block cache initialized");

    vfs_init();
    bool have_root_fs = vfs_mount_root_auto();
    if (!have_root_fs) {
        log_error("vfs", "Root filesystem mount failed; entering recovery mode");
        print(NULL, "\n[vfs] root mount failed; entering recovery shell\n");
    }
    if (!devfs_mount_at("/dev")) {
        log_error("devfs", "Failed to mount /dev");
    }

    if (have_root_fs) {
        static const char* const init_candidates[] = { "/bin/init", "/init" };
        const char* launched_path = NULL;
        process_t* init = NULL;

        for (uint32_t i = 0; i < (sizeof(init_candidates) / sizeof(init_candidates[0])); i++) {
            init = kxe_load(init_candidates[i]);
            if (init) {
                launched_path = init_candidates[i];
                break;
            }
        }

        if (init) {
            log_infof("init", "Launching userspace %s", launched_path);
            scheduler_run(init);
            log_info("init", "Userspace session ended; falling back to kernel shell");
            print(NULL, "\n[init] userspace session ended; entering kernel shell\n");
        } else {
            log_error("init", "Failed to load /bin/init or /init; entering kernel shell");
        }
    }

    shell_loop(fb);
    
    // We should never return here; halt if we do.
    while (1) { asm volatile ("hlt"); }
}
