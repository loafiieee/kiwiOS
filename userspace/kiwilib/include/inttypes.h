#ifndef KIWILIB_INTTYPES_H
#define KIWILIB_INTTYPES_H

#include <stdint.h>

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

#define PRId8  "d"
#define PRIi8  "i"
#define PRIu8  "u"
#define PRIo8  "o"
#define PRIx8  "x"
#define PRIX8  "X"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIo16 "o"
#define PRIx16 "x"
#define PRIX16 "X"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIo32 "o"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "ld"
#define PRIi64 "li"
#define PRIu64 "lu"
#define PRIo64 "lo"
#define PRIx64 "lx"
#define PRIX64 "lX"

#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIuMAX "lu"
#define PRIoMAX "lo"
#define PRIxMAX "lx"
#define PRIXMAX "lX"
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIoPTR "lo"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#define SCNd8  "hhd"
#define SCNi8  "hhi"
#define SCNu8  "hhu"
#define SCNo8  "hho"
#define SCNx8  "hhx"
#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNu16 "hu"
#define SCNo16 "ho"
#define SCNx16 "hx"
#define SCNd32 "d"
#define SCNi32 "i"
#define SCNu32 "u"
#define SCNo32 "o"
#define SCNx32 "x"
#define SCNd64 "ld"
#define SCNi64 "li"
#define SCNu64 "lu"
#define SCNo64 "lo"
#define SCNx64 "lx"

#define SCNdMAX "ld"
#define SCNiMAX "li"
#define SCNuMAX "lu"
#define SCNoMAX "lo"
#define SCNxMAX "lx"
#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNuPTR "lu"
#define SCNoPTR "lo"
#define SCNxPTR "lx"

intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);
intmax_t strtoimax(const char* nptr, char** endptr, int base);
uintmax_t strtoumax(const char* nptr, char** endptr, int base);

#endif // KIWILIB_INTTYPES_H
