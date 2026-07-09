#ifndef KIWILIB_LANGINFO_H
#define KIWILIB_LANGINFO_H

typedef int nl_item;

#define CODESET 1
#define D_T_FMT 2
#define D_FMT 3
#define T_FMT 4
#define RADIXCHAR 5
#define THOUSEP 6
#define AM_STR 7
#define PM_STR 8
#define DAY_1 20
#define DAY_2 21
#define DAY_3 22
#define DAY_4 23
#define DAY_5 24
#define DAY_6 25
#define DAY_7 26
#define ABDAY_1 30
#define ABDAY_2 31
#define ABDAY_3 32
#define ABDAY_4 33
#define ABDAY_5 34
#define ABDAY_6 35
#define ABDAY_7 36
#define MON_1 40
#define MON_2 41
#define MON_3 42
#define MON_4 43
#define MON_5 44
#define MON_6 45
#define MON_7 46
#define MON_8 47
#define MON_9 48
#define MON_10 49
#define MON_11 50
#define MON_12 51
#define ABMON_1 60
#define ABMON_2 61
#define ABMON_3 62
#define ABMON_4 63
#define ABMON_5 64
#define ABMON_6 65
#define ABMON_7 66
#define ABMON_8 67
#define ABMON_9 68
#define ABMON_10 69
#define ABMON_11 70
#define ABMON_12 71
#define YESEXPR 80
#define NOEXPR 81

char* nl_langinfo(nl_item item);

#endif // KIWILIB_LANGINFO_H
