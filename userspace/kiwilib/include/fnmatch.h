#ifndef KIWILIB_FNMATCH_H
#define KIWILIB_FNMATCH_H

#define FNM_NOMATCH 1
#define FNM_PATHNAME 0x01
#define FNM_NOESCAPE 0x02
#define FNM_PERIOD 0x04
#define FNM_CASEFOLD 0x08

int fnmatch(const char* pattern, const char* string, int flags);

#endif // KIWILIB_FNMATCH_H
