#ifndef KIWILIB_REGEX_H
#define KIWILIB_REGEX_H

#include <stddef.h>

typedef ptrdiff_t regoff_t;

typedef struct {
    char pattern[256];
    int cflags;
    int errcode;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8
#define REG_NOTBOL   1
#define REG_NOTEOL   2
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_EBRACK   3
#define REG_EESCAPE  4
#define REG_ESPACE   5

int regcomp(regex_t* preg, const char* pattern, int cflags);
int regexec(const regex_t* preg, const char* string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size);
void regfree(regex_t* preg);

#endif // KIWILIB_REGEX_H
