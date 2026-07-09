#ifndef KIWILIB_SYS_WAIT_H
#define KIWILIB_SYS_WAIT_H

#include "sys/types.h"

#define WNOHANG 1
#define WUNTRACED 2

#define WIFEXITED(status) (1)
#define WEXITSTATUS(status) (status)
#define WIFSIGNALED(status) (0)
#define WTERMSIG(status) (0)
#define WIFSTOPPED(status) (0)
#define WSTOPSIG(status) (0)

pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);

#endif // KIWILIB_SYS_WAIT_H
