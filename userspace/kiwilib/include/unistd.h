#ifndef KIWILIB_UNISTD_H
#define KIWILIB_UNISTD_H

#include <stddef.h>
#include "sys/types.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 3
#define _SC_PAGESIZE 4
#define _SC_PAGE_SIZE _SC_PAGESIZE

#define _PC_PIPE_BUF 0

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
int pipe(int pipefd[2]);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char* path);
int unlinkat(int dirfd, const char* path, int flags);
int rmdir(const char* path);
int link(const char* oldpath, const char* newpath);
int linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags);
int symlink(const char* target, const char* linkpath);
int symlinkat(const char* target, int newdirfd, const char* linkpath);
ssize_t readlink(const char* path, char* buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz);
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int access(const char* path, int mode);
int faccessat(int dirfd, const char* path, int mode, int flags);
int truncate(const char* path, off_t length);
int ftruncate(int fd, off_t length);
int fsync(int fd);
void sync(void);
pid_t fork(void);
int execv(const char* path, char* const argv[]);
int execvp(const char* file, char* const argv[]);
int execl(const char* path, const char* arg, ...);
int execlp(const char* file, const char* arg, ...);
int execle(const char* path, const char* arg, ...);
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t setsid(void);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getgroups(int size, gid_t list[]);
int setuid(uid_t uid);
int seteuid(uid_t euid);
int setgid(gid_t gid);
int setegid(gid_t egid);
int chown(const char* path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int lchown(const char* path, uid_t owner, gid_t group);
int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags);
void* sbrk(intptr_t increment);
int isatty(int fd);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
char* ttyname(int fd);
int ttyname_r(int fd, char* buf, size_t buflen);
int getpagesize(void);
long sysconf(int name);
long fpathconf(int fd, int name);
long pathconf(const char* path, int name);
int gethostname(char* name, size_t len);
int getdtablesize(void);
char* getlogin(void);
int getlogin_r(char* name, size_t size);
extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;
int getopt(int argc, char* const argv[], const char* optstring);

#endif // KIWILIB_UNISTD_H
