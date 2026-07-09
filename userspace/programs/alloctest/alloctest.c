#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char* a = (char*)malloc(32);
    char* b = NULL;
    char* c = NULL;

    puts("alloctest: starting");

    if (!a) {
        puts("alloctest: FAIL malloc");
        return 1;
    }

    memcpy(a, "kiwi malloc", 12);
    b = (char*)realloc(a, 128);
    if (!b || memcmp(b, "kiwi malloc", 12) != 0) {
        puts("alloctest: FAIL realloc");
        return 2;
    }

    c = (char*)calloc(16, 4);
    if (!c) {
        puts("alloctest: FAIL calloc");
        return 3;
    }
    for (int i = 0; i < 64; i++) {
        if (c[i] != 0) {
            puts("alloctest: FAIL calloc zero");
            return 4;
        }
    }

    free(b);
    free(c);
    puts("alloctest: PASS malloc/calloc/realloc/free");
    return 0;
}
