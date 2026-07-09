#ifndef KIWILIB_SETJMP_H
#define KIWILIB_SETJMP_H

typedef long jmp_buf[8];
typedef long sigjmp_buf[8];

int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int value) __attribute__((noreturn));
void siglongjmp(sigjmp_buf env, int value) __attribute__((noreturn));

#define sigsetjmp(env, savesigs) ((void)(savesigs), setjmp(env))

#endif // KIWILIB_SETJMP_H
