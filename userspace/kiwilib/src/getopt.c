#include <stddef.h>
#include "getopt.h"
#include "string.h"

char* optarg;
int optind = 1;
int opterr = 1;
int optopt;

static char* g_next_short;

static const char* find_short_option(const char* optstring, int c) {
    if (!optstring) {
        return NULL;
    }
    if (*optstring == ':' || *optstring == '+') {
        optstring++;
    }
    while (*optstring) {
        if (*optstring == c) {
            return optstring;
        }
        optstring++;
        if (*optstring == ':') {
            optstring++;
            if (*optstring == ':') {
                optstring++;
            }
        }
    }
    return NULL;
}

int getopt(int argc, char* const argv[], const char* optstring) {
    const char* opt = NULL;
    int c;

    optarg = NULL;
    if (optind <= 0) {
        optind = 1;
    }

    if (!g_next_short || *g_next_short == '\0') {
        char* arg;

        if (optind >= argc) {
            return -1;
        }

        arg = argv[optind];
        if (!arg || arg[0] != '-' || arg[1] == '\0') {
            return -1;
        }
        if (arg[1] == '-' && arg[2] == '\0') {
            optind++;
            return -1;
        }

        g_next_short = arg + 1;
    }

    c = (unsigned char)*g_next_short++;
    opt = find_short_option(optstring, c);
    if (!opt) {
        optopt = c;
        if (*g_next_short == '\0') {
            optind++;
            g_next_short = NULL;
        }
        return '?';
    }

    if (opt[1] == ':') {
        int optional = opt[2] == ':';
        if (*g_next_short != '\0') {
            optarg = g_next_short;
            optind++;
            g_next_short = NULL;
        } else if (!optional && optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
            g_next_short = NULL;
        } else {
            optind++;
            g_next_short = NULL;
            if (!optional) {
                optopt = c;
                return optstring && optstring[0] == ':' ? ':' : '?';
            }
        }
    } else if (*g_next_short == '\0') {
        optind++;
        g_next_short = NULL;
    }

    return c;
}

static int long_name_matches(const char* arg, const char* name, const char** value_out) {
    size_t name_len = strlen(name);

    if (strncmp(arg, name, name_len) != 0) {
        return 0;
    }
    if (arg[name_len] == '\0') {
        *value_out = NULL;
        return 1;
    }
    if (arg[name_len] == '=') {
        *value_out = arg + name_len + 1u;
        return 1;
    }
    return 0;
}

static int parse_long_arg(char* arg,
                          const char* optstring,
                          const struct option* longopts,
                          int* longindex,
                          int argc,
                          char* const argv[]) {
    const char* value = NULL;

    for (int i = 0; longopts && longopts[i].name; i++) {
        if (!long_name_matches(arg, longopts[i].name, &value)) {
            continue;
        }

        if (longindex) {
            *longindex = i;
        }

        if (longopts[i].has_arg == no_argument) {
            if (value) {
                optind++;
                return '?';
            }
        } else if (longopts[i].has_arg == required_argument) {
            if (value) {
                optarg = (char*)value;
            } else if (optind + 1 < argc) {
                optarg = argv[++optind];
            } else {
                optind++;
                return optstring && optstring[0] == ':' ? ':' : '?';
            }
        } else if (longopts[i].has_arg == optional_argument) {
            optarg = (char*)value;
        }

        optind++;
        if (longopts[i].flag) {
            *longopts[i].flag = longopts[i].val;
            return 0;
        }
        return longopts[i].val;
    }

    optind++;
    return '?';
}

int getopt_long(int argc,
                char* const argv[],
                const char* optstring,
                const struct option* longopts,
                int* longindex) {
    char* arg;

    if (optind <= 0) {
        optind = 1;
    }

    optarg = NULL;
    if (optind >= argc) {
        return -1;
    }

    arg = argv[optind];
    if (arg && arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
        return parse_long_arg(arg + 2, optstring, longopts, longindex, argc, argv);
    }

    return getopt(argc, argv, optstring);
}

int getopt_long_only(int argc,
                     char* const argv[],
                     const char* optstring,
                     const struct option* longopts,
                     int* longindex) {
    char* arg;

    if (optind <= 0) {
        optind = 1;
    }
    optarg = NULL;
    if (optind >= argc) {
        return -1;
    }

    arg = argv[optind];
    if (arg && arg[0] == '-' && arg[1] != '-' && arg[1] != '\0') {
        for (int i = 0; longopts && longopts[i].name; i++) {
            const char* value = NULL;
            if (long_name_matches(arg + 1, longopts[i].name, &value)) {
                return parse_long_arg(arg + 1, optstring, longopts, longindex, argc, argv);
            }
        }
    }
    return getopt_long(argc, argv, optstring, longopts, longindex);
}
