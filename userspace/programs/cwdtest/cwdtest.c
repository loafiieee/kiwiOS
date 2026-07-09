#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char* msg, int code) {
    puts(msg);
    return code;
}

static int write_all(int fd, const char* s) {
    size_t len = strlen(s);
    return write(fd, s, len) == (ssize_t)len ? 0 : -1;
}

int main(void) {
    char cwd[256];
    char buf[32];
    struct stat st;
    int fd = -1;
    ssize_t n = 0;

    puts("cwdtest: starting");

    if (!getcwd(cwd, sizeof(cwd)) || cwd[0] != '/') {
        return fail("cwdtest: FAIL getcwd", 1);
    }

    (void)unlink("cwdtest.tmp/two.txt");
    (void)unlink("cwdtest.tmp/one.txt");
    (void)rmdir("cwdtest.tmp");

    if (mkdir("cwdtest.tmp", 0755) != 0) {
        return fail("cwdtest: FAIL mkdir relative", 2);
    }
    if (chdir("cwdtest.tmp") != 0) {
        return fail("cwdtest: FAIL chdir relative", 3);
    }
    if (!getcwd(cwd, sizeof(cwd)) || !strstr(cwd, "/cwdtest.tmp")) {
        return fail("cwdtest: FAIL getcwd after chdir", 4);
    }

    fd = open("one.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return fail("cwdtest: FAIL open relative", 5);
    }
    if (write_all(fd, "abcdef") != 0) {
        close(fd);
        return fail("cwdtest: FAIL write", 6);
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 6) {
        close(fd);
        return fail("cwdtest: FAIL fstat", 7);
    }
    if (ftruncate(fd, 3) != 0) {
        close(fd);
        return fail("cwdtest: FAIL ftruncate", 8);
    }
    close(fd);

    if (rename("one.txt", "two.txt") != 0) {
        return fail("cwdtest: FAIL rename", 9);
    }
    if (access("two.txt", R_OK) != 0 || stat("two.txt", &st) != 0 || st.st_size != 3) {
        return fail("cwdtest: FAIL access/stat", 10);
    }

    if (truncate("two.txt", 2) != 0 || stat("two.txt", &st) != 0 || st.st_size != 2) {
        return fail("cwdtest: FAIL truncate", 11);
    }

    fd = open("two.txt", O_RDONLY);
    if (fd < 0) {
        return fail("cwdtest: FAIL reopen", 12);
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != 2 || strcmp(buf, "ab") != 0) {
        return fail("cwdtest: FAIL readback", 13);
    }

    if (chdir("..") != 0) {
        return fail("cwdtest: FAIL chdir parent", 14);
    }
    if (unlink("cwdtest.tmp/two.txt") != 0 || rmdir("cwdtest.tmp") != 0) {
        return fail("cwdtest: FAIL cleanup", 15);
    }

    puts("cwdtest: PASS cwd/access/stat/fstat/rename/truncate");
    return 0;
}
