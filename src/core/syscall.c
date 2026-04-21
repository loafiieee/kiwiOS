#include <stddef.h>
#include <stdint.h>
#include "core/syscall.h"
#include "core/console.h"
#include "core/process.h"
#include "memory/vmm.h"

#define USER_VADDR_MIN 0x0000000000001000ull
#define USER_VADDR_MAX 0x0000800000000000ull

bool validate_user_buffer(uint64_t addr, uint64_t len, bool needs_write) {
    process_t* proc = process_current();
    uint64_t end = 0;
    uint64_t page = 0;
    uint64_t last_page = 0;

    if (len == 0) {
        return true;
    }

    if (!proc || !proc->page_table) {
        return false;
    }

    if (addr < USER_VADDR_MIN || addr >= USER_VADDR_MAX) {
        return false;
    }

    end = addr + len;
    if (end < addr || end > USER_VADDR_MAX) {
        return false;
    }

    page = PAGE_ALIGN_DOWN(addr);
    last_page = PAGE_ALIGN_DOWN(end - 1u);

    for (;;) {
        uint64_t flags = 0;

        if (!vmm_get_mapping(proc->page_table, page, NULL, &flags)) {
            return false;
        }

        if (!(flags & PAGE_USER)) {
            return false;
        }

        if (needs_write && !(flags & PAGE_WRITE)) {
            return false;
        }

        if (page == last_page) {
            break;
        }

        if (page > (UINT64_MAX - PAGE_SIZE)) {
            return false;
        }
        page += PAGE_SIZE;
    }

    return true;
}

int64_t sys_write(int fd, const void* buf, uint64_t len) {
    if ((fd != 1 && fd != 2) || (!buf && len != 0)) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)buf, len, false)) {
        return -1;
    }

    const char* s = (const char*)buf;
    for (uint64_t i = 0; i < len; i++) {
        putc_fb(NULL, s[i]);
    }

    return (int64_t)len;
}

void sys_exit(int code) {
    process_t* proc = process_current();
    if (proc) {
        proc->state = PROC_ZOMBIE;
        proc->exit_code = code;

        print(NULL, "\n[sys_exit] pid=");
        print_u32(NULL, proc->pid);
        print(NULL, " exited with code ");
        print_u64(NULL, (uint64_t)(uint32_t)code);
        print(NULL, "\n");
    } else {
        print(NULL, "\n[sys_exit] Process exited with code ");
        print_u64(NULL, (uint64_t)(uint32_t)code);
        print(NULL, "\n");
    }

    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

int64_t sys_getpid(void) {
    process_t* proc = process_current();
    if (!proc) {
        return -1;
    }

    return (int64_t)proc->pid;
}

int64_t syscall_dispatch(syscall_frame_t* frame) {
    if (!frame) {
        return -1;
    }

    switch (frame->syscall_num) {
        case KIWI_SYS_EXIT:
            sys_exit((int)frame->rdi);
        case KIWI_SYS_WRITE:
            return sys_write((int)frame->rdi, (const void*)frame->rsi, frame->rdx);
        case KIWI_SYS_GETPID:
            return sys_getpid();
        default:
            return -1;
    }
}
