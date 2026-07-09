#ifndef KIWILIB_STDIO_H
#define KIWILIB_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include "sys/types.h"

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256
#define L_tmpnam 32
#define TMP_MAX 10000
#define P_tmpdir "/tmp"

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef struct FILE FILE;
typedef long fpos_t;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int putchar(int ch);
int puts(const char* s);
int fputc(int ch, FILE* stream);
int putc(int ch, FILE* stream);
int fputs(const char* s, FILE* stream);
int fputs_unlocked(const char* s, FILE* stream);
int fflush(FILE* stream);
int fflush_unlocked(FILE* stream);
FILE* fopen(const char* path, const char* mode);
FILE* fdopen(int fd, const char* mode);
FILE* freopen(const char* path, const char* mode, FILE* stream);
FILE* tmpfile(void);
char* tmpnam(char* s);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fgetc(FILE* stream);
int getc(FILE* stream);
int getchar(void);
char* fgets(char* s, int size, FILE* stream);
ssize_t getline(char** lineptr, size_t* n, FILE* stream);
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream);
int ungetc(int ch, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fseeko(FILE* stream, long offset, int whence);
long ftello(FILE* stream);
int fgetpos(FILE* stream, fpos_t* pos);
int fsetpos(FILE* stream, const fpos_t* pos);
void rewind(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
void __fseterr(FILE* stream);
int fileno(FILE* stream);
int getc_unlocked(FILE* stream);
int getchar_unlocked(void);
int putc_unlocked(int ch, FILE* stream);
int putchar_unlocked(int ch);
int setvbuf(FILE* stream, char* buf, int mode, size_t size);
void setbuf(FILE* stream, char* buf);
int printf(const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int fprintf(FILE* stream, const char* fmt, ...);
int vfprintf(FILE* stream, const char* fmt, va_list ap);
int dprintf(int fd, const char* fmt, ...);
int vdprintf(int fd, const char* fmt, va_list ap);
int sprintf(char* buf, const char* fmt, ...);
int vsprintf(char* buf, const char* fmt, va_list ap);
int scanf(const char* fmt, ...);
int vscanf(const char* fmt, va_list ap);
int fscanf(FILE* stream, const char* fmt, ...);
int vfscanf(FILE* stream, const char* fmt, va_list ap);
int sscanf(const char* str, const char* fmt, ...);
int vsscanf(const char* str, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int asprintf(char** strp, const char* fmt, ...);
int vasprintf(char** strp, const char* fmt, va_list ap);
void perror(const char* s);
int remove(const char* path);
int rename(const char* old_path, const char* new_path);
int renameat(int olddirfd, const char* old_path, int newdirfd, const char* new_path);

#endif // KIWILIB_STDIO_H
