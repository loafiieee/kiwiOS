#include <stdarg.h>
#include <stdint.h>
#include "abi/kiwi.h"
#include "errno.h"
#include "fcntl.h"
#include "kiwi_syscall.h"
#include "sys/ioctl.h"
#include "sys/file.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include "sys/statvfs.h"
#include "sys/time.h"
#include "sys/types.h"
#include "sys/uio.h"
#include "sys/wait.h"
#include "stdlib.h"
#include "termios.h"
#include "utime.h"
#include "unistd.h"
#include "string.h"

int errno;

typedef struct {
    void* ptr;
    size_t length;
    int prot;
    int flags;
} mmap_region_t;

static mmap_region_t g_mmap_regions[32];

static int kiwi_fail(int err) {
    errno = err;
    return -1;
}

static ssize_t kiwi_fail_ssize(int err) {
    errno = err;
    return -1;
}

static int at_path_supported(int dirfd, const char* path) {
    if (!path) {
        errno = EINVAL;
        return 0;
    }
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        return 1;
    }
    errno = ENOSYS;
    return 0;
}

static void stat_from_kiwi(const kiwi_stat_t* in, struct stat* out) {
    memset(out, 0, sizeof(*out));
    out->st_mode = in->mode;
    if (in->type == KIWI_VNODE_FILE) {
        out->st_mode |= S_IFREG;
    } else if (in->type == KIWI_VNODE_DIR) {
        out->st_mode |= S_IFDIR;
    } else if (in->type == KIWI_VNODE_PIPE) {
        out->st_mode |= S_IFIFO;
    }
    out->st_ino = in->ino;
    out->st_size = in->size;
    out->st_nlink = in->link_count;
    out->st_blksize = 4096;
    out->st_blocks = (in->size + 511u) / 512u;
    out->st_uid = 0;
    out->st_gid = 0;
    out->st_atime = in->mtime;
    out->st_mtime = in->mtime;
    out->st_ctime = in->ctime;
}

int open(const char* path, int flags, ...) {
    int64_t fd;
    int kiwi_flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND);

    if (!path) {
        return kiwi_fail(EINVAL);
    }

    if ((flags & O_CREAT) != 0) {
        va_list ap;
        va_start(ap, flags);
        (void)va_arg(ap, int);
        va_end(ap);
    }

    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) && access(path, F_OK) == 0) {
        return kiwi_fail(EEXIST);
    }

    fd = sys_open(path, kiwi_flags);
    if (fd < 0) {
        return kiwi_fail(EIO);
    }

    if ((flags & O_CLOEXEC) != 0 && fcntl((int)fd, F_SETFD, FD_CLOEXEC) < 0) {
        close((int)fd);
        return -1;
    }

    return (int)fd;
}

int openat(int dirfd, const char* path, int flags, ...) {
    int mode = 0;

    if ((flags & O_CREAT) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return open(path, flags, mode);
}

int creat(const char* path, mode_t mode) {
    return open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

ssize_t read(int fd, void* buf, size_t count) {
    int64_t n;

    n = sys_read(fd, buf, (uint64_t)count);
    if (n < 0) {
        return kiwi_fail_ssize(EIO);
    }

    return (ssize_t)n;
}

ssize_t write(int fd, const void* buf, size_t count) {
    int64_t n;

    n = sys_write(fd, buf, (uint64_t)count);
    if (n < 0) {
        return kiwi_fail_ssize(EIO);
    }

    return (ssize_t)n;
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    ssize_t total = 0;

    if (!iov || iovcnt < 0) {
        return kiwi_fail_ssize(EINVAL);
    }
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n;
        if (!iov[i].iov_base && iov[i].iov_len != 0) {
            return kiwi_fail_ssize(EINVAL);
        }
        n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : -1;
        }
        total += n;
        if ((size_t)n != iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    ssize_t total = 0;

    if (!iov || iovcnt < 0) {
        return kiwi_fail_ssize(EINVAL);
    }
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n;
        if (!iov[i].iov_base && iov[i].iov_len != 0) {
            return kiwi_fail_ssize(EINVAL);
        }
        n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : -1;
        }
        total += n;
        if ((size_t)n != iov[i].iov_len) {
            break;
        }
    }
    return total;
}

int close(int fd) {
    if (sys_close(fd) < 0) {
        return kiwi_fail(EBADF);
    }
    return 0;
}

int dup(int oldfd) {
    int64_t fd = sys_dup(oldfd);
    if (fd < 0) {
        return kiwi_fail(EBADF);
    }
    return (int)fd;
}

int dup2(int oldfd, int newfd) {
    int64_t fd = sys_dup2(oldfd, newfd);
    if (fd < 0) {
        return kiwi_fail(EBADF);
    }
    return (int)fd;
}

int dup3(int oldfd, int newfd, int flags) {
    int fd;

    if (oldfd == newfd || (flags & ~O_CLOEXEC) != 0) {
        return kiwi_fail(EINVAL);
    }
    fd = dup2(oldfd, newfd);
    if (fd < 0) {
        return -1;
    }
    if ((flags & O_CLOEXEC) != 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int pipe(int pipefd[2]) {
    if (!pipefd) {
        return kiwi_fail(EINVAL);
    }
    if (sys_pipe(pipefd) < 0) {
        return kiwi_fail(EMFILE);
    }
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    uint64_t arg = 0;
    int64_t ret;

    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        struct flock* lock = NULL;
        va_start(ap, cmd);
        lock = va_arg(ap, struct flock*);
        va_end(ap);
        if (cmd == F_GETLK && lock) {
            lock->l_type = F_UNLCK;
        }
        return 0;
    }

    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC || cmd == F_SETFD || cmd == F_SETFL) {
        va_start(ap, cmd);
        arg = (uint64_t)va_arg(ap, int);
        va_end(ap);
    }

    if (cmd == F_DUPFD_CLOEXEC) {
        ret = sys_fcntl(fd, F_DUPFD, arg);
        if (ret < 0) {
            return kiwi_fail(EBADF);
        }
        (void)sys_fcntl((int)ret, F_SETFD, FD_CLOEXEC);
        return (int)ret;
    }

    ret = sys_fcntl(fd, cmd, arg);
    if (ret < 0) {
        return kiwi_fail(EBADF);
    }
    return (int)ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    int64_t new_offset;

    new_offset = sys_seek(fd, (int64_t)offset, whence);
    if (new_offset < 0) {
        return (off_t)kiwi_fail(EINVAL);
    }

    return (off_t)new_offset;
}

int stat(const char* path, struct stat* out) {
    kiwi_stat_t st;

    if (!path || !out) {
        return kiwi_fail(EINVAL);
    }

    if (sys_stat(path, &st) < 0) {
        return kiwi_fail(ENOENT);
    }

    stat_from_kiwi(&st, out);
    return 0;
}

int lstat(const char* path, struct stat* out) {
    return stat(path, out);
}

int fstat(int fd, struct stat* out) {
    kiwi_stat_t st;

    if (!out) {
        return kiwi_fail(EINVAL);
    }

    if (sys_fstat(fd, &st) < 0) {
        return kiwi_fail(EBADF);
    }

    stat_from_kiwi(&st, out);
    return 0;
}

int fstatat(int dirfd, const char* path, struct stat* out, int flags) {
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0) {
        return kiwi_fail(EINVAL);
    }
    if ((flags & AT_EMPTY_PATH) != 0 && path && path[0] == '\0') {
        return fstat(dirfd, out);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(path, out) : stat(path, out);
}

static void statvfs_fill(struct statvfs* buf) {
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 0;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
    buf->f_files = 0;
    buf->f_ffree = 0;
    buf->f_favail = 0;
    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = 64;
}

int statvfs(const char* path, struct statvfs* buf) {
    struct stat st;

    if (!path || !buf) {
        return kiwi_fail(EINVAL);
    }
    if (stat(path, &st) != 0) {
        return -1;
    }
    statvfs_fill(buf);
    return 0;
}

int fstatvfs(int fd, struct statvfs* buf) {
    struct stat st;

    if (!buf) {
        return kiwi_fail(EINVAL);
    }
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    statvfs_fill(buf);
    return 0;
}

int mkdir(const char* path, mode_t mode) {
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    if (sys_mkdir(path, mode) < 0) {
        return kiwi_fail(EIO);
    }
    return 0;
}

int mkdirat(int dirfd, const char* path, mode_t mode) {
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return mkdir(path, mode);
}

int chmod(const char* path, mode_t mode) {
    struct stat st;
    (void)mode;
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    return stat(path, &st);
}

int fchmod(int fd, mode_t mode) {
    struct stat st;
    (void)mode;
    return fstat(fd, &st);
}

int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        return kiwi_fail(EINVAL);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return chmod(path, mode);
}

int chown(const char* path, uid_t owner, gid_t group) {
    struct stat st;
    (void)owner;
    (void)group;
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    return stat(path, &st);
}

int fchown(int fd, uid_t owner, gid_t group) {
    struct stat st;
    (void)owner;
    (void)group;
    return fstat(fd, &st);
}

int lchown(const char* path, uid_t owner, gid_t group) {
    return chown(path, owner, group);
}

int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags) {
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        return kiwi_fail(EINVAL);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return chown(path, owner, group);
}

mode_t umask(mode_t mask) {
    static mode_t current = 022;
    mode_t old = current;
    current = mask;
    return old;
}

int unlink(const char* path) {
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    if (sys_unlink(path) < 0) {
        return kiwi_fail(EIO);
    }
    return 0;
}

int rmdir(const char* path) {
    return unlink(path);
}

int unlinkat(int dirfd, const char* path, int flags) {
    if ((flags & ~AT_REMOVEDIR) != 0) {
        return kiwi_fail(EINVAL);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int link(const char* oldpath, const char* newpath) {
    (void)oldpath;
    (void)newpath;
    return kiwi_fail(ENOSYS);
}

int linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags) {
    (void)flags;
    if (!at_path_supported(olddirfd, oldpath) || !at_path_supported(newdirfd, newpath)) {
        return -1;
    }
    return link(oldpath, newpath);
}

int symlink(const char* target, const char* linkpath) {
    (void)target;
    (void)linkpath;
    return kiwi_fail(ENOSYS);
}

int symlinkat(const char* target, int newdirfd, const char* linkpath) {
    if (!at_path_supported(newdirfd, linkpath)) {
        return -1;
    }
    return symlink(target, linkpath);
}

ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
    (void)path;
    (void)buf;
    (void)bufsiz;
    return kiwi_fail_ssize(EINVAL);
}

ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz) {
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return readlink(path, buf, bufsiz);
}

int rename(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) {
        return kiwi_fail(EINVAL);
    }
    if (sys_rename(old_path, new_path) < 0) {
        return kiwi_fail(EIO);
    }
    return 0;
}

int renameat(int olddirfd, const char* old_path, int newdirfd, const char* new_path) {
    if (!at_path_supported(olddirfd, old_path) || !at_path_supported(newdirfd, new_path)) {
        return -1;
    }
    return rename(old_path, new_path);
}

pid_t fork(void) {
    return (pid_t)kiwi_fail(ENOSYS);
}

static int argv_count(char* const argv[]) {
    int argc = 0;

    if (!argv) {
        return 0;
    }

    while (argv[argc]) {
        argc++;
    }
    return argc;
}

int execv(const char* path, char* const argv[]) {
    int argc = argv_count(argv);

    if (!path) {
        return kiwi_fail(EINVAL);
    }

    if (argc == 0) {
        const char* fallback[2];
        fallback[0] = path;
        fallback[1] = NULL;
        if (sys_exec_argv(path, 1, fallback) < 0) {
            return kiwi_fail(ENOENT);
        }
    } else if (sys_exec_argv(path, argc, (const char* const*)argv) < 0) {
        return kiwi_fail(ENOENT);
    }

    __builtin_unreachable();
}

int execvp(const char* file, char* const argv[]) {
    char path[256];
    size_t len = 0;

    if (!file || !*file) {
        return kiwi_fail(EINVAL);
    }

    if (strchr(file, '/')) {
        return execv(file, argv);
    }

    len = strlen(file);
    if (len + 6u < sizeof(path)) {
        memcpy(path, "/bin/", 5);
        memcpy(path + 5, file, len + 1u);
        (void)execv(path, argv);
    }
    if (len + 2u < sizeof(path)) {
        path[0] = '/';
        memcpy(path + 1, file, len + 1u);
        (void)execv(path, argv);
    }

    return kiwi_fail(ENOENT);
}

static int execv_from_varargs(const char* path, const char* arg, va_list ap, int search_path, int has_envp) {
    char* argv[64];
    size_t argc = 0;
    const char* cur = arg;

    if (!path) {
        return kiwi_fail(EINVAL);
    }

    while (cur) {
        if (argc + 1u >= sizeof(argv) / sizeof(argv[0])) {
            return kiwi_fail(E2BIG);
        }
        argv[argc++] = (char*)cur;
        cur = va_arg(ap, const char*);
    }

    if (has_envp) {
        (void)va_arg(ap, char* const*);
    }

    if (argc == 0) {
        argv[argc++] = (char*)path;
    }
    argv[argc] = NULL;

    return search_path ? execvp(path, argv) : execv(path, argv);
}

int execl(const char* path, const char* arg, ...) {
    va_list ap;
    int ret;

    va_start(ap, arg);
    ret = execv_from_varargs(path, arg, ap, 0, 0);
    va_end(ap);
    return ret;
}

int execlp(const char* file, const char* arg, ...) {
    va_list ap;
    int ret;

    va_start(ap, arg);
    ret = execv_from_varargs(file, arg, ap, 1, 0);
    va_end(ap);
    return ret;
}

int execle(const char* path, const char* arg, ...) {
    va_list ap;
    int ret;

    va_start(ap, arg);
    ret = execv_from_varargs(path, arg, ap, 0, 1);
    va_end(ap);
    return ret;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    int64_t ret;

    if (options != 0) {
        return (pid_t)kiwi_fail(ENOSYS);
    }

    ret = sys_waitpid((int)pid, status);
    if (ret < 0) {
        return (pid_t)kiwi_fail(ECHILD);
    }
    return (pid_t)ret;
}

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}

int chdir(const char* path) {
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    if (sys_chdir(path) < 0) {
        return kiwi_fail(ENOENT);
    }
    return 0;
}

char* getcwd(char* buf, size_t size) {
    if (!buf || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (sys_getcwd(buf, (uint64_t)size) < 0) {
        errno = EIO;
        return NULL;
    }
    return buf;
}

int access(const char* path, int mode) {
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    if (sys_access(path, mode) < 0) {
        return kiwi_fail(ENOENT);
    }
    return 0;
}

int faccessat(int dirfd, const char* path, int mode, int flags) {
    if ((flags & ~AT_EACCESS) != 0) {
        return kiwi_fail(EINVAL);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return access(path, mode);
}

int truncate(const char* path, off_t length) {
    if (!path || length < 0) {
        return kiwi_fail(EINVAL);
    }
    if (sys_truncate(path, (uint64_t)length) < 0) {
        return kiwi_fail(EIO);
    }
    return 0;
}

int ftruncate(int fd, off_t length) {
    if (length < 0) {
        return kiwi_fail(EINVAL);
    }
    if (sys_ftruncate(fd, (uint64_t)length) < 0) {
        return kiwi_fail(EIO);
    }
    return 0;
}

int fsync(int fd) {
    if (fcntl(fd, F_GETFL) < 0) {
        return -1;
    }
    return 0;
}

void sync(void) {
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void* ptr = NULL;

    if (length == 0 || offset != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if ((flags & MAP_FIXED) != 0 || addr != NULL) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    if ((flags & MAP_ANONYMOUS) == 0 || fd != -1) {
        errno = ENOSYS;
        return MAP_FAILED;
    }

    ptr = calloc(1, length);
    if (!ptr) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    for (size_t i = 0; i < sizeof(g_mmap_regions) / sizeof(g_mmap_regions[0]); i++) {
        if (!g_mmap_regions[i].ptr) {
            g_mmap_regions[i].ptr = ptr;
            g_mmap_regions[i].length = length;
            g_mmap_regions[i].prot = prot;
            g_mmap_regions[i].flags = flags;
            return ptr;
        }
    }

    free(ptr);
    errno = ENOMEM;
    return MAP_FAILED;
}

int munmap(void* addr, size_t length) {
    if (!addr || length == 0) {
        return kiwi_fail(EINVAL);
    }

    for (size_t i = 0; i < sizeof(g_mmap_regions) / sizeof(g_mmap_regions[0]); i++) {
        if (g_mmap_regions[i].ptr == addr) {
            free(addr);
            memset(&g_mmap_regions[i], 0, sizeof(g_mmap_regions[i]));
            return 0;
        }
    }

    return kiwi_fail(EINVAL);
}

int mprotect(void* addr, size_t len, int prot) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + len;

    if (!addr || len == 0 || end < start) {
        return kiwi_fail(EINVAL);
    }

    for (size_t i = 0; i < sizeof(g_mmap_regions) / sizeof(g_mmap_regions[0]); i++) {
        uintptr_t region_start;
        uintptr_t region_end;

        if (!g_mmap_regions[i].ptr) {
            continue;
        }

        region_start = (uintptr_t)g_mmap_regions[i].ptr;
        region_end = region_start + g_mmap_regions[i].length;
        if (start >= region_start && end <= region_end) {
            g_mmap_regions[i].prot = prot;
            return 0;
        }
    }

    return kiwi_fail(EINVAL);
}

int msync(void* addr, size_t length, int flags) {
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

int flock(int fd, int operation) {
    (void)operation;
    if (fcntl(fd, F_GETFL) < 0) {
        return -1;
    }
    return 0;
}

int utime(const char* path, const struct utimbuf* times) {
    struct stat st;
    (void)times;
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    return stat(path, &st);
}

int utimes(const char* path, const struct timeval times[2]) {
    struct stat st;
    (void)times;
    if (!path) {
        return kiwi_fail(EINVAL);
    }
    return stat(path, &st);
}

int futimens(int fd, const struct timespec times[2]) {
    struct stat st;
    (void)times;
    return fstat(fd, &st);
}

int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) {
    struct stat st;
    (void)times;
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        return kiwi_fail(EINVAL);
    }
    if (!at_path_supported(dirfd, path)) {
        return -1;
    }
    return stat(path, &st);
}

pid_t getpid(void) {
    int64_t pid = sys_getpid();
    if (pid < 0) {
        errno = EIO;
        return -1;
    }
    return (pid_t)pid;
}

pid_t getppid(void) {
    return 1;
}

pid_t getpgrp(void) {
    return getpid();
}

int setpgid(pid_t pid, pid_t pgid) {
    pid_t self = getpid();

    if ((pid != 0 && pid != self) || (pgid != 0 && pgid != self)) {
        return kiwi_fail(EPERM);
    }
    return 0;
}

pid_t setsid(void) {
    return getpid();
}

pid_t tcgetpgrp(int fd) {
    if (!isatty(fd)) {
        return (pid_t)-1;
    }
    return getpgrp();
}

int tcsetpgrp(int fd, pid_t pgrp) {
    if (pgrp <= 0) {
        return kiwi_fail(EINVAL);
    }
    return isatty(fd) ? 0 : -1;
}

void* sbrk(intptr_t increment) {
    uint64_t cur;
    uint64_t next;

    cur = (uint64_t)sys_brk(0);
    if ((int64_t)cur < 0) {
        errno = ENOMEM;
        return (void*)-1;
    }

    next = (uint64_t)((intptr_t)cur + increment);
    if ((increment > 0 && next < cur) || (increment < 0 && next > cur)) {
        errno = ENOMEM;
        return (void*)-1;
    }

    if ((uint64_t)sys_brk(next) != next) {
        errno = ENOMEM;
        return (void*)-1;
    }

    return (void*)(uintptr_t)cur;
}

int isatty(int fd) {
    struct winsize ws;

    if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        return 1;
    }

    errno = ENOTTY;
    return 0;
}

char* ttyname(int fd) {
    static char name[] = "/dev/console";
    return isatty(fd) ? name : NULL;
}

int ttyname_r(int fd, char* buf, size_t buflen) {
    const char* name = "/dev/console";
    size_t len = strlen(name) + 1u;

    if (!buf || buflen == 0) {
        return EINVAL;
    }
    if (!isatty(fd)) {
        return ENOTTY;
    }
    if (len > buflen) {
        return ERANGE;
    }
    memcpy(buf, name, len);
    return 0;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    void* arg = 0;

    va_start(ap, request);
    arg = va_arg(ap, void*);
    va_end(ap);

    if (sys_ioctl(fd, (uint64_t)request, arg) < 0) {
        return kiwi_fail(ENOTTY);
    }

    return 0;
}

int tcgetattr(int fd, struct termios* termios_p) {
    if (!termios_p) {
        return kiwi_fail(EINVAL);
    }

    return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios* termios_p) {
    unsigned long request = TCSETS;

    if (!termios_p) {
        return kiwi_fail(EINVAL);
    }

    if (optional_actions == TCSADRAIN) {
        request = TCSETSW;
    } else if (optional_actions == TCSAFLUSH) {
        request = TCSETSF;
    } else if (optional_actions != TCSANOW) {
        return kiwi_fail(EINVAL);
    }

    return ioctl(fd, request, (void*)termios_p);
}

void cfmakeraw(struct termios* termios_p) {
    if (!termios_p) {
        return;
    }

    termios_p->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag |= CS8;
    termios_p->c_cc[VMIN] = 1;
    termios_p->c_cc[VTIME] = 0;
}

int tcdrain(int fd) {
    return isatty(fd) ? 0 : kiwi_fail(ENOTTY);
}

int tcflush(int fd, int queue_selector) {
    if (queue_selector != TCIFLUSH && queue_selector != TCOFLUSH && queue_selector != TCIOFLUSH) {
        return kiwi_fail(EINVAL);
    }
    return isatty(fd) ? 0 : kiwi_fail(ENOTTY);
}

int tcflow(int fd, int action) {
    if (action != TCOOFF && action != TCOON && action != TCIOFF && action != TCION) {
        return kiwi_fail(EINVAL);
    }
    return isatty(fd) ? 0 : kiwi_fail(ENOTTY);
}

speed_t cfgetispeed(const struct termios* termios_p) {
    (void)termios_p;
    return B38400;
}

speed_t cfgetospeed(const struct termios* termios_p) {
    (void)termios_p;
    return B38400;
}

static int termios_speed_valid(speed_t speed) {
    switch (speed) {
        case B0:
        case B50:
        case B75:
        case B110:
        case B134:
        case B150:
        case B200:
        case B300:
        case B600:
        case B1200:
        case B1800:
        case B2400:
        case B4800:
        case B9600:
        case B19200:
        case B38400:
        case B57600:
        case B115200:
        case B230400:
            return 1;
        default:
            return 0;
    }
}

int cfsetispeed(struct termios* termios_p, speed_t speed) {
    if (!termios_p) {
        return kiwi_fail(EINVAL);
    }
    if (!termios_speed_valid(speed)) {
        return kiwi_fail(EINVAL);
    }
    return 0;
}

int cfsetospeed(struct termios* termios_p, speed_t speed) {
    if (!termios_p) {
        return kiwi_fail(EINVAL);
    }
    if (!termios_speed_valid(speed)) {
        return kiwi_fail(EINVAL);
    }
    return 0;
}

int cfsetspeed(struct termios* termios_p, speed_t speed) {
    if (cfsetispeed(termios_p, speed) != 0) {
        return -1;
    }
    if (cfsetospeed(termios_p, speed) != 0) {
        return -1;
    }
    return 0;
}
