#include "ctype.h"

int isspace(int c) {
    return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}

int isblank(int c) {
    return c == ' ' || c == '\t';
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int islower(int c) {
    return c >= 'a' && c <= 'z';
}

int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

int isalpha(int c) {
    return islower(c) || isupper(c);
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int iscntrl(int c) {
    return (c >= 0 && c < 32) || c == 127;
}

int isprint(int c) {
    return c >= 32 && c < 127;
}

int isascii(int c) {
    return c >= 0 && c < 128;
}

int isgraph(int c) {
    return c > 32 && c < 127;
}

int ispunct(int c) {
    return isgraph(c) && !isalnum(c);
}

int tolower(int c) {
    return isupper(c) ? c + ('a' - 'A') : c;
}

int toupper(int c) {
    return islower(c) ? c - ('a' - 'A') : c;
}

int toascii(int c) {
    return c & 0x7f;
}
