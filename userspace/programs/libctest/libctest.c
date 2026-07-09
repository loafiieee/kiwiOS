#include <assert.h>
#include <alloca.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <grp.h>
#include <getopt.h>
#include <inttypes.h>
#include <iso646.h>
#include "kiwi_syscall.h"
#include <langinfo.h>
#include <libgen.h>
#include <locale.h>
#include <pwd.h>
#include <poll.h>
#include <regex.h>
#include <setjmp.h>
#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <term.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

static int g_failure_count;
static int g_first_failure_code;

static void record_fail(const char* msg, int code) {
    if (g_failure_count == 0) {
        g_first_failure_code = code;
    }
    g_failure_count++;
    puts(msg);
}

static volatile sig_atomic_t g_saw_signal;
static jmp_buf g_jump;

static void test_signal_handler(int signum) {
    if (signum == SIGUSR1) {
        g_saw_signal = 1;
    }
}

static int compare_ints(const void* a, const void* b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

static int exercise_setjmp(int value) {
    int ret = setjmp(g_jump);
    if (ret == 0) {
        longjmp(g_jump, value);
    }
    return ret;
}

int main(int argc, char** argv, char** envp) {
    char buf[128];
    char* end = NULL;
    char* opt_argv[] = {
        "libctest",
        "-n",
        "42",
        "--mode=fast",
        NULL,
    };
    struct option opts[] = {
        { "mode", required_argument, NULL, 'm' },
        { NULL, 0, NULL, 0 },
    };
    int opt_argc = 4;
    int got_n = 0;
    int got_mode = 0;
    int c;
    FILE* fp = NULL;
    DIR* dir = NULL;
    int saw_self = 0;
    int fd = -1;
    int dup_fd = -1;
    int saved_stdout = -1;
    char token_buf[32];
    char* saveptr = NULL;
    regex_t regex;
    regmatch_t match;
    glob_t glob_result;
    struct dirent** scan_list = NULL;
    int scan_count = 0;
    struct utsname uts;
    int status = -1;
    int64_t child = -1;
    wchar_t wc = 0;
    wchar_t wcbuf[16];
    const char* mbsrc = NULL;
    const wchar_t* wcsrc = NULL;
    char mbbuf[16];
    int pipefd[2] = { -1, -1 };
    struct pollfd pfd;
    fd_set rfds;
    struct stat st;
    fpos_t fpos;
    struct iovec iov[2];
    struct rlimit rlim;
    struct rusage usage;
    struct statvfs svfs;
    sigset_t sigset;
    sigset_t old_sigset;
    sigset_t pending_sigset;
    struct timespec ts;
    struct timespec res;
    struct itimerval itv;
    struct tms tms_buf;
    struct tm parsed_tm;
    struct tm* tm;
    time_t sample_time = 0;
    char* parsed_end = NULL;
    struct lconv* lc = NULL;
    imaxdiv_t imax_pair;
    int nums[4] = { 4, 1, 3, 2 };
    int key = 3;
    int* found_num = NULL;
    char temp_path[64];
    char real_buf[256];
    char path_buf_a[64];
    char path_buf_b[64];
    char* dup_str = NULL;
    char* line = NULL;
    size_t line_cap = 0;
    char* fmt_alloc = NULL;
    char env_put[] = "KIWI_PUTENV=put";
    char subopt_buf[] = "width=80,readonly";
    char* subopt_ptr = subopt_buf;
    char* subopt_value = NULL;
    char* const subopt_tokens[] = { "width", "readonly", NULL };
    char host_buf[16];
    char login_buf[16];
    gid_t groups[2];
    char* stack_buf = NULL;
    char sep_buf[16];
    char* sep_ptr = NULL;
    int* int_array = NULL;
    void* map = NULL;
    char copy_buf[32];
    char collate_buf[16];
    wchar_t wcollate_buf[16];
    int scan_i = 0;
    unsigned int scan_x = 0;
    int scan_n = 0;
    char scan_word[16];
    char scan_set[16];
    char scan_chars[3] = { 0, 0, 0 };

    puts("libctest: starting");
    (void)argc;
    (void)argv;
    assert(sizeof(char) == 1);
    if ((not false) != true || alignof(int) < alignof(char)) {
        record_fail("libctest: FAIL iso headers", 94);
    }
    if (exercise_setjmp(42) != 42 || exercise_setjmp(0) != 1) {
        record_fail("libctest: FAIL setjmp", 92);
    }

    if (!envp || !envp[0] || !getenv("TERM") || strcmp(getenv("TERM"), "xterm") != 0) {
        record_fail("libctest: FAIL envp/getenv", 1);
    }
    if (setenv("KIWI_TEST_ENV", "one", 1) != 0 ||
        strcmp(getenv("KIWI_TEST_ENV"), "one") != 0 ||
        setenv("KIWI_TEST_ENV", "two", 0) != 0 ||
        strcmp(secure_getenv("KIWI_TEST_ENV"), "one") != 0 ||
        setenv("KIWI_TEST_ENV", "two", 1) != 0 ||
        strcmp(getenv("KIWI_TEST_ENV"), "two") != 0 ||
        putenv(env_put) != 0 ||
        strcmp(getenv("KIWI_PUTENV"), "put") != 0 ||
        unsetenv("KIWI_TEST_ENV") != 0 ||
        getenv("KIWI_TEST_ENV") != NULL) {
        record_fail("libctest: FAIL env mutation", 2);
    }
    if (getsubopt(&subopt_ptr, subopt_tokens, &subopt_value) != 0 ||
        !subopt_value || strcmp(subopt_value, "80") != 0 ||
        getsubopt(&subopt_ptr, subopt_tokens, &subopt_value) != 1) {
        record_fail("libctest: FAIL getsubopt", 110);
    }
    if (getpagesize() != 4096 || sysconf(_SC_PAGESIZE) != 4096 ||
        sysconf(_SC_OPEN_MAX) < 3 ||
        gethostname(host_buf, sizeof(host_buf)) != 0 ||
        strcmp(host_buf, "kiwi") != 0 ||
        getdtablesize() < 3 ||
        !getlogin() || strcmp(getlogin(), "root") != 0 ||
        getlogin_r(login_buf, sizeof(login_buf)) != 0 ||
        strcmp(login_buf, "root") != 0 ||
        getgroups(2, groups) != 1 || groups[0] != 0 ||
        setuid(0) != 0 || seteuid(0) != 0 ||
        setgid(0) != 0 || setegid(0) != 0) {
        record_fail("libctest: FAIL sysconf/hostname", 3);
    }
    stack_buf = (char*)alloca(8);
    strcpy(stack_buf, "stk");
    if (strcmp(stack_buf, "stk") != 0 ||
        getppid() != 1 || getpgrp() != getpid() ||
        setpgid(0, 0) != 0 || setsid() != getpid() ||
        tcgetpgrp(STDOUT_FILENO) != getpid() ||
        tcsetpgrp(STDOUT_FILENO, getpid()) != 0) {
        record_fail("libctest: FAIL process/session stubs", 80);
    }

    if (snprintf(buf, sizeof(buf), "fmt:%s:%d:%x:%zu", "ok", -7, 0x2a, (size_t)9) < 0) {
        record_fail("libctest: FAIL snprintf return", 4);
    }
    if (strcmp(buf, "fmt:ok:-7:2a:9") != 0) {
        record_fail("libctest: FAIL snprintf contents", 5);
    }
    if (snprintf(buf, sizeof(buf), "%*.*s:%#o:%+d", 5, 2, "abcd", 8, 7) < 0 ||
        strcmp(buf, "   ab:010:+7") != 0) {
        record_fail("libctest: FAIL snprintf dynamic", 6);
    }
    if (sscanf("n=-12 hex=2a word=kiwi", "n=%d hex=%x word=%15s%n",
               &scan_i, &scan_x, scan_word, &scan_n) != 3 ||
        scan_i != -12 || scan_x != 0x2a || strcmp(scan_word, "kiwi") != 0 || scan_n <= 0 ||
        sscanf("abc123", "%[a-z]%d", scan_set, &scan_i) != 2 ||
        strcmp(scan_set, "abc") != 0 || scan_i != 123 ||
        sscanf("xy", "%2c", scan_chars) != 1 ||
        scan_chars[0] != 'x' || scan_chars[1] != 'y') {
        record_fail("libctest: FAIL sscanf", 93);
    }

    if (strtol("0x2a!", &end, 0) != 42 || !end || *end != '!') {
        record_fail("libctest: FAIL strtol", 7);
    }
    imax_pair = imaxdiv((intmax_t)17, (intmax_t)5);
    if (!true || false ||
        strtoimax("-42!", &end, 10) != (intmax_t)-42 || !end || *end != '!' ||
        strtoumax("2a!", &end, 16) != (uintmax_t)42 || !end || *end != '!' ||
        imaxabs((intmax_t)-9) != (intmax_t)9 ||
        imax_pair.quot != (intmax_t)3 || imax_pair.rem != (intmax_t)2 ||
        snprintf(buf, sizeof(buf), "%" PRIdMAX ":%" PRIuMAX, (intmax_t)-7, (uintmax_t)9) < 0 ||
        strcmp(buf, "-7:9") != 0) {
        record_fail("libctest: FAIL inttypes", 91);
    }
    if (strtod("3.25!", &end) < 3.249 || strtod("3.25!", &end) > 3.251 || !end || *end != '!') {
        record_fail("libctest: FAIL strtod", 8);
    }
    if (strcasecmp("KiWi", "kiwi") != 0 || strncasecmp("Shell", "she", 3) != 0 ||
        strspn("abc123", "abc") != 3 || strcspn("abc123", "123") != 3 ||
        !strpbrk("abc", "xcy") || !strcasestr("KiwiOS", "wio") ||
        strcmp(strerror(ENOENT), "No such file or directory") != 0 ||
        memchr("abc", 'b', 3) == NULL) {
        record_fail("libctest: FAIL string helpers", 9);
    }
    strcpy(buf, "abc");
    if (strlcpy(buf, "abcdef", 4) != 6 || strcmp(buf, "abc") != 0 ||
        strlcat(buf, "XYZ", sizeof(buf)) != 6 || strcmp(buf, "abcXYZ") != 0 ||
        memrchr("abca", 'a', 4) != ((const char*)"abca" + 3) ||
        rawmemchr("xyz", 'z') != ((const char*)"xyz" + 2) ||
        *strchrnul("abc", 'z') != '\0') {
        record_fail("libctest: FAIL string extensions", 86);
    }
    memset(copy_buf, 0, sizeof(copy_buf));
    if (mempcpy(copy_buf, "abc", 3) != copy_buf + 3 ||
        strcmp(stpcpy(copy_buf + 3, "def") - 6, "abcdef") != 0 ||
        stpncpy(copy_buf, "xy", 5) != copy_buf + 2 ||
        copy_buf[2] != '\0' ||
        strcoll("abc", "abd") >= 0 ||
        strxfrm(collate_buf, "kiwi", sizeof(collate_buf)) != 4 ||
        strcmp(collate_buf, "kiwi") != 0 ||
        strverscmp("file9", "file10") >= 0) {
        record_fail("libctest: FAIL string port helpers", 95);
    }
    strcpy(sep_buf, "a,b");
    sep_ptr = sep_buf;
    if (strcmp(strsep(&sep_ptr, ","), "a") != 0 ||
        strcmp(strsep(&sep_ptr, ","), "b") != 0 ||
        strsep(&sep_ptr, ",") != NULL) {
        record_fail("libctest: FAIL strsep", 87);
    }
    memset(buf, 0x5a, sizeof(buf));
    explicit_bzero(buf, 8);
    if (buf[0] != 0 || buf[7] != 0 || buf[8] != 0x5a) {
        record_fail("libctest: FAIL explicit_bzero", 88);
    }
    int_array = (int*)reallocarray(NULL, 4, sizeof(int));
    if (!int_array) {
        record_fail("libctest: FAIL reallocarray alloc", 89);
    }
    int_array[3] = 42;
    int_array = (int*)reallocarray(int_array, 8, sizeof(int));
    if (!int_array || int_array[3] != 42) {
        free(int_array);
        record_fail("libctest: FAIL reallocarray grow", 90);
    }
    free(int_array);
    if (fnmatch("*.c", "hello.c", 0) != 0 ||
        fnmatch("*.c", "hello.h", 0) != FNM_NOMATCH ||
        fnmatch("SRC/*.C", "src/main.c", FNM_PATHNAME | FNM_CASEFOLD) != 0 ||
        fnmatch("*.c", "dir/main.c", FNM_PATHNAME) != FNM_NOMATCH) {
        record_fail("libctest: FAIL fnmatch", 79);
    }
    dup_str = strndup("abcdef", 3);
    if (!dup_str || strcmp(dup_str, "abc") != 0 ||
        strerror_r(ENOENT, buf, sizeof(buf)) != 0 || strcmp(buf, "No such file or directory") != 0 ||
        strcmp(strsignal(SIGINT), "Interrupt") != 0) {
        free(dup_str);
        record_fail("libctest: FAIL string compat", 10);
    }
    free(dup_str);
    strcpy(token_buf, "one,two");
    if (strcmp(strtok_r(token_buf, ",", &saveptr), "one") != 0 ||
        strcmp(strtok_r(NULL, ",", &saveptr), "two") != 0 ||
        strtok_r(NULL, ",", &saveptr) != NULL) {
        record_fail("libctest: FAIL strtok_r", 11);
    }
    qsort(nums, 4, sizeof(nums[0]), compare_ints);
    found_num = (int*)bsearch(&key, nums, 4, sizeof(nums[0]), compare_ints);
    if (!found_num || *found_num != 3 || nums[0] != 1 || nums[3] != 4) {
        record_fail("libctest: FAIL qsort/bsearch", 12);
    }

    optind = 1;
    while ((c = getopt_long(opt_argc, opt_argv, "n:", opts, NULL)) != -1) {
        if (c == 'n') {
            if (!optarg || atoi(optarg) != 42) {
                record_fail("libctest: FAIL getopt short arg", 76);
            }
            got_n = 1;
        } else if (c == 'm') {
            if (!optarg || strcmp(optarg, "fast") != 0) {
                record_fail("libctest: FAIL getopt long arg", 77);
            }
            got_mode = 1;
        } else {
            record_fail("libctest: FAIL getopt unexpected", 13);
        }
    }

    if (!got_n || !got_mode) {
        record_fail("libctest: FAIL getopt missing option", 14);
    }
    {
        char* long_only_argv[] = { "libctest", "-mode=slow", NULL };
        optind = 1;
        c = getopt_long_only(2, long_only_argv, "", opts, NULL);
        if (c != 'm' || !optarg || strcmp(optarg, "slow") != 0) {
            record_fail("libctest: FAIL getopt long only", 96);
        }
    }

    fp = fopen("libctest.tmp", "w+");
    if (!fp) {
        record_fail("libctest: FAIL fopen", 15);
    }
    if (fwrite("line1\nline2\n", 1, 12, fp) != 12) {
        fclose(fp);
        record_fail("libctest: FAIL fwrite", 16);
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        record_fail("libctest: FAIL fseek", 17);
    }
    memset(buf, 0, sizeof(buf));
    if (!fgets(buf, sizeof(buf), fp) || strcmp(buf, "line1\n") != 0) {
        fclose(fp);
        record_fail("libctest: FAIL fgets", 18);
    }
    if (fgetpos(fp, &fpos) != 0 || fseeko(fp, 0, SEEK_END) != 0 ||
        ftello(fp) != 12 || fsetpos(fp, &fpos) != 0 ||
        getc_unlocked(fp) != 'l') {
        fclose(fp);
        record_fail("libctest: FAIL stream position", 19);
    }
    if (fclose(fp) != 0) {
        record_fail("libctest: FAIL fclose", 20);
    }
    if (remove("libctest.tmp") != 0) {
        record_fail("libctest: FAIL remove temp", 21);
    }
    fp = tmpfile();
    if (!fp || putc_unlocked('q', fp) != 'q' || fseek(fp, 0, SEEK_SET) != 0 || fgetc(fp) != 'q') {
        if (fp) {
            fclose(fp);
        }
        record_fail("libctest: FAIL tmpfile", 22);
    }
    fclose(fp);
    strcpy(temp_path, "libctest.mks.XXXXXX");
    fd = mkstemp(temp_path);
    if (fd < 0 || write(fd, "t", 1) != 1 || close(fd) != 0 || unlink(temp_path) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        record_fail("libctest: FAIL mkstemp", 23);
    }
    strcpy(temp_path, "libctest.dir.XXXXXX");
    if (!mkdtemp(temp_path) || rmdir(temp_path) != 0) {
        record_fail("libctest: FAIL mkdtemp", 24);
    }
    if (!realpath("/bin", real_buf) || strcmp(real_buf, "/bin") != 0 || system(NULL) != 0) {
        record_fail("libctest: FAIL realpath/system", 25);
    }
    fd = openat(AT_FDCWD, "libctest.at", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0 || write(fd, "at", 2) != 2 ||
        fstatat(AT_FDCWD, "libctest.at", &st, 0) != 0 || !S_ISREG(st.st_mode) ||
        faccessat(AT_FDCWD, "libctest.at", R_OK | W_OK, AT_EACCESS) != 0 ||
        fchmodat(AT_FDCWD, "libctest.at", 0644, 0) != 0 ||
        chown("libctest.at", 0, 0) != 0 ||
        fchown(fd, 0, 0) != 0 ||
        futimens(fd, NULL) != 0 ||
        close(fd) != 0 ||
        renameat(AT_FDCWD, "libctest.at", AT_FDCWD, "libctest.at2") != 0 ||
        utimensat(AT_FDCWD, "libctest.at2", NULL, 0) != 0 ||
        unlinkat(AT_FDCWD, "libctest.at2", 0) != 0 ||
        mkdirat(AT_FDCWD, "libctest.atdir", 0700) != 0 ||
        unlinkat(AT_FDCWD, "libctest.atdir", AT_REMOVEDIR) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        unlink("libctest.at");
        unlink("libctest.at2");
        rmdir("libctest.atdir");
        record_fail("libctest: FAIL at wrappers", 85);
    }

    fd = open("libctest.fdopen", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        record_fail("libctest: FAIL fdopen open", 26);
    }
    if (dprintf(fd, "dprintf:%d\n", 12) < 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        record_fail("libctest: FAIL dprintf", 27);
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 11) != 11 || strcmp(buf, "dprintf:12\n") != 0 ||
        ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        record_fail("libctest: FAIL dprintf read", 28);
    }
    fp = fdopen(fd, "w+");
    if (!fp) {
        close(fd);
        record_fail("libctest: FAIL fdopen", 29);
    }
    if (fputs("stream\n", fp) == EOF || fseek(fp, 0, SEEK_SET) != 0 ||
        !fgets(buf, sizeof(buf), fp) || strcmp(buf, "stream\n") != 0) {
        fclose(fp);
        record_fail("libctest: FAIL fdopen io", 30);
    }
    if (fseek(fp, 0, SEEK_SET) != 0 || getline(&line, &line_cap, fp) != 7 ||
        strcmp(line, "stream\n") != 0) {
        free(line);
        fclose(fp);
        record_fail("libctest: FAIL getline", 31);
    }
    free(line);
    line = NULL;
    if (asprintf(&fmt_alloc, "alloc:%s:%d", "ok", 3) != 10 ||
        !fmt_alloc || strcmp(fmt_alloc, "alloc:ok:3") != 0) {
        free(fmt_alloc);
        fclose(fp);
        record_fail("libctest: FAIL asprintf", 32);
    }
    free(fmt_alloc);
    fmt_alloc = NULL;
    if (fclose(fp) != 0 || remove("libctest.fdopen") != 0) {
        record_fail("libctest: FAIL fdopen cleanup", 33);
    }

    dir = opendir("/bin");
    if (!dir) {
        record_fail("libctest: FAIL opendir", 34);
    }
    for (;;) {
        struct dirent* ent = readdir(dir);
        if (!ent) {
            break;
        }
        if (strcmp(ent->d_name, "libctest") == 0) {
            saw_self = 1;
        }
    }
    closedir(dir);
    if (!saw_self) {
        record_fail("libctest: FAIL readdir", 35);
    }
    dir = opendir("/bin");
    if (!dir) {
        record_fail("libctest: FAIL dir position open", 106);
    }
    {
        struct dirent* ent = readdir(dir);
        long pos = telldir(dir);
        char saved_name[64];

        (void)ent;
        ent = readdir(dir);
        if (!ent || pos < 0) {
            closedir(dir);
            record_fail("libctest: FAIL telldir", 107);
        }
        strcpy(saved_name, ent->d_name);
        seekdir(dir, pos);
        ent = readdir(dir);
        if (!ent || strcmp(ent->d_name, saved_name) != 0 ||
            dirfd(dir) != -1 || errno != ENOTSUP) {
            closedir(dir);
            record_fail("libctest: FAIL seekdir/dirfd", 108);
        }
    }
    closedir(dir);
    scan_count = scandir("/bin", &scan_list, NULL, alphasort);
    if (scan_count <= 0 || !scan_list) {
        record_fail("libctest: FAIL scandir", 97);
    }
    saw_self = 0;
    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_list[i]->d_name, "libctest") == 0) {
            saw_self = 1;
        }
        free(scan_list[i]);
    }
    free(scan_list);
    scan_list = NULL;
    if (!saw_self) {
        record_fail("libctest: FAIL scandir contents", 98);
    }

    fd = open("libctest.dup", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        record_fail("libctest: FAIL dup open", 36);
    }
    dup_fd = dup(fd);
    if (dup_fd < 0) {
        close(fd);
        record_fail("libctest: FAIL dup", 37);
    }
    if (write(fd, "ab", 2) != 2 || write(dup_fd, "cd", 2) != 2) {
        close(dup_fd);
        close(fd);
        record_fail("libctest: FAIL dup write", 38);
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(dup_fd);
        close(fd);
        record_fail("libctest: FAIL dup seek", 39);
    }
    memset(buf, 0, sizeof(buf));
    if (read(dup_fd, buf, 4) != 4 || strcmp(buf, "abcd") != 0) {
        close(dup_fd);
        close(fd);
        record_fail("libctest: FAIL dup shared offset/read", 40);
    }
    if (fcntl(fd, F_GETFL) < 0 || fcntl(fd, F_SETFL, O_APPEND) != 0) {
        close(dup_fd);
        close(fd);
        record_fail("libctest: FAIL fcntl", 41);
    }
    {
        int clo_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (clo_fd < 0 || fcntl(clo_fd, F_GETFD) != FD_CLOEXEC || close(clo_fd) != 0) {
            close(dup_fd);
            close(fd);
            record_fail("libctest: FAIL fcntl cloexec", 82);
        }
    }
    iov[0].iov_base = "ef";
    iov[0].iov_len = 2;
    iov[1].iov_base = "gh";
    iov[1].iov_len = 2;
    if (writev(fd, iov, 2) != 4 || lseek(fd, 0, SEEK_SET) != 0) {
        close(dup_fd);
        close(fd);
        record_fail("libctest: FAIL writev", 42);
    }
    memset(buf, 0, sizeof(buf));
    iov[0].iov_base = buf;
    iov[0].iov_len = 4;
    iov[1].iov_base = buf + 4;
    iov[1].iov_len = 4;
    if (readv(fd, iov, 2) != 8 || strcmp(buf, "abcdefgh") != 0) {
        close(dup_fd);
        close(fd);
        record_fail("libctest: FAIL readv", 43);
    }
    close(dup_fd);
    close(fd);
    if (unlink("libctest.dup") != 0) {
        record_fail("libctest: FAIL dup unlink", 44);
    }

    saved_stdout = dup(STDOUT_FILENO);
    fd = open("libctest.redirect", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (saved_stdout < 0 || fd < 0 || dup2(fd, STDOUT_FILENO) != STDOUT_FILENO) {
        if (fd >= 0) {
            close(fd);
        }
        if (saved_stdout >= 0) {
            close(saved_stdout);
        }
        record_fail("libctest: FAIL dup2 setup", 45);
    }
    printf("redirect-ok\n");
    if (dup2(saved_stdout, STDOUT_FILENO) != STDOUT_FILENO) {
        record_fail("libctest: FAIL dup2 restore", 46);
    }
    close(saved_stdout);
    if (isatty(fd)) {
        close(fd);
        record_fail("libctest: FAIL isatty redirected file", 47);
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        record_fail("libctest: FAIL redirect seek", 48);
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, sizeof("redirect-ok\n") - 1u) != (ssize_t)(sizeof("redirect-ok\n") - 1u) ||
        strcmp(buf, "redirect-ok\n") != 0) {
        close(fd);
        record_fail("libctest: FAIL redirect read", 49);
    }
    close(fd);
    if (unlink("libctest.redirect") != 0) {
        record_fail("libctest: FAIL redirect unlink", 50);
    }

    if (!getpwuid(0) || strcmp(getpwuid(0)->pw_dir, "/home") != 0 ||
        !getpwnam("root") || !getgrgid(0) || !getgrnam("root")) {
        record_fail("libctest: FAIL passwd/group", 51);
    }
    if (uname(&uts) != 0 || strcmp(uts.sysname, "KiwiOS") != 0 || strcmp(uts.machine, "x86_64") != 0) {
        record_fail("libctest: FAIL uname", 52);
    }
    if (stat("/bin/libctest", &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != 0 || st.st_gid != 0 || st.st_blksize != 4096) {
        record_fail("libctest: FAIL stat fields", 78);
    }
    if (statvfs("/bin", &svfs) != 0 || svfs.f_bsize != 4096 || svfs.f_namemax < 64) {
        record_fail("libctest: FAIL statvfs", 81);
    }
    lc = localeconv();
    if (!lc || strcmp(setlocale(LC_ALL, ""), "C") != 0 ||
        strcmp(lc->decimal_point, ".") != 0 ||
        strcmp(nl_langinfo(CODESET), "ASCII") != 0 ||
        strcmp(nl_langinfo(MON_1), "January") != 0 ||
        strcmp(nl_langinfo(ABDAY_1), "Sun") != 0) {
        record_fail("libctest: FAIL locale/langinfo", 53);
    }
    strcpy(path_buf_a, "/tmp/example.txt");
    strcpy(path_buf_b, "/tmp/example.txt");
    if (strcmp(basename(path_buf_a), "example.txt") != 0 ||
        strcmp(dirname(path_buf_b), "/tmp") != 0) {
        record_fail("libctest: FAIL basename/dirname", 54);
    }
    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0 || rlim.rlim_cur < 3 ||
        getrusage(RUSAGE_SELF, &usage) != 0) {
        record_fail("libctest: FAIL resource/mmap", 55);
    }
    map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        record_fail("libctest: FAIL mmap anon", 55);
    }
    strcpy((char*)map, "mapped");
    if (strcmp((char*)map, "mapped") != 0 ||
        mprotect(map, 4096, PROT_READ) != 0 ||
        msync(map, 4096, MS_SYNC) != 0 ||
        munmap(map, 4096) != 0) {
        record_fail("libctest: FAIL mmap lifecycle", 55);
    }
    if (munmap(map, 4096) == 0 || errno != EINVAL) {
        record_fail("libctest: FAIL mmap errors", 55);
    }
    if (!ttyname(STDOUT_FILENO) || strcmp(ttyname(STDOUT_FILENO), "/dev/console") != 0 ||
        ttyname_r(STDOUT_FILENO, buf, sizeof(buf)) != 0 || strcmp(buf, "/dev/console") != 0) {
        record_fail("libctest: FAIL ttyname", 56);
    }
    if (sigemptyset(&sigset) != 0 || sigaddset(&sigset, SIGUSR1) != 0 ||
        sigismember(&sigset, SIGUSR1) != 1 || sigprocmask(SIG_BLOCK, &sigset, &old_sigset) != 0) {
        record_fail("libctest: FAIL signal mask", 57);
    }
    if (sigpending(&pending_sigset) != 0 || pending_sigset != 0 ||
        siginterrupt(SIGUSR1, 1) != 0) {
        record_fail("libctest: FAIL signal helpers", 109);
    }
    (void)signal(SIGUSR1, test_signal_handler);
    g_saw_signal = 0;
    if (raise(SIGUSR1) != 0 || g_saw_signal != 0 ||
        sigprocmask(SIG_UNBLOCK, &sigset, NULL) != 0 ||
        raise(SIGUSR1) != 0 || g_saw_signal != 1 ||
        kill(getpid(), 0) != 0) {
        record_fail("libctest: FAIL signal raise", 58);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || time(NULL) == (time_t)-1) {
        record_fail("libctest: FAIL time basics", 59);
    }
    res.tv_sec = 0;
    res.tv_nsec = 1000000L;
    if (nanosleep(&res, NULL) != 0) {
        record_fail("libctest: FAIL time sleep", 59);
    }
    if (clock_getres(CLOCK_REALTIME, &res) != 0 || res.tv_nsec <= 0 ||
        timespec_get(&ts, TIME_UTC) != TIME_UTC ||
        getitimer(ITIMER_REAL, &itv) != 0 ||
        setitimer(ITIMER_REAL, &itv, NULL) != 0 ||
        times(&tms_buf) == (clock_t)-1) {
        record_fail("libctest: FAIL time extensions", 99);
    }
    res.tv_sec = 0;
    res.tv_nsec = 1000000L;
    if (clock_nanosleep(CLOCK_MONOTONIC, 0, &res, NULL) != 0) {
        record_fail("libctest: FAIL clock_nanosleep", 99);
    }
    memset(&parsed_tm, 0, sizeof(parsed_tm));
    parsed_end = strptime("2026-07-05 12:34:56Z", "%F %T", &parsed_tm);
    if (!parsed_end || *parsed_end != 'Z' ||
        parsed_tm.tm_year != 126 || parsed_tm.tm_mon != 6 ||
        parsed_tm.tm_mday != 5 || parsed_tm.tm_hour != 12 ||
        parsed_tm.tm_min != 34 || parsed_tm.tm_sec != 56) {
        record_fail("libctest: FAIL strptime", 100);
    }
    sample_time = 0;
    tm = gmtime(&sample_time);
    if (!tm || tm->tm_year != 70 || tm->tm_mon != 0 || tm->tm_mday != 1 ||
        tm->tm_wday != 4 || tm->tm_yday != 0 ||
        strftime(buf, sizeof(buf), "%F %T %a %b %j %w %u %z %Z", tm) == 0 ||
        strcmp(buf, "1970-01-01 00:00:00 Thu Jan 001 4 4 +0000 UTC") != 0 ||
        strcmp(asctime(tm), "Thu Jan  1 00:00:00 1970\n") != 0 ||
        strcmp(ctime(&sample_time), "Thu Jan  1 00:00:00 1970\n") != 0) {
        record_fail("libctest: FAIL epoch time conversion", 60);
    }
    sample_time = 1783254896;
    tm = gmtime(&sample_time);
    if (!tm || tm->tm_year != 126 || tm->tm_mon != 6 || tm->tm_mday != 5 ||
        tm->tm_hour != 12 || tm->tm_min != 34 || tm->tm_sec != 56 ||
        tm->tm_wday != 0 || tm->tm_yday != 185 ||
        strftime(buf, sizeof(buf), "%c", tm) == 0 ||
        strcmp(buf, "Sun Jul  5 12:34:56 2026") != 0) {
        record_fail("libctest: FAIL calendar time conversion", 60);
    }
    if (mktime(&parsed_tm) != 1783254896 || parsed_tm.tm_wday != 0 || parsed_tm.tm_yday != 185) {
        record_fail("libctest: FAIL mktime", 60);
    }
    tm = gmtime(&ts.tv_sec);
    if (!tm || strftime(buf, sizeof(buf), "%F %T", tm) == 0) {
        record_fail("libctest: FAIL strftime", 60);
    }
    if (regcomp(&regex, "userspace", 0) != 0 ||
        regexec(&regex, "hello userspace", 1, &match, 0) != 0 ||
        match.rm_so != 6 || match.rm_eo != 15) {
        record_fail("libctest: FAIL regex", 61);
    }
    regfree(&regex);
    if (regcomp(&regex, "^h.llo[[:space:]]+[a-z]+$", REG_EXTENDED) != 0 ||
        regexec(&regex, "hello userspace", 1, &match, 0) != 0 ||
        match.rm_so != 0 || match.rm_eo != 15) {
        record_fail("libctest: FAIL regex extended", 61);
    }
    regfree(&regex);
    if (regcomp(&regex, "^[^0-9]*[0-9][0-9]?$", REG_EXTENDED) != 0 ||
        regexec(&regex, "port42", 1, &match, 0) != 0 ||
        match.rm_so != 0 || match.rm_eo != 6) {
        record_fail("libctest: FAIL regex classes", 61);
    }
    regfree(&regex);
    if (regcomp(&regex, "^second", REG_NEWLINE) != 0 ||
        regexec(&regex, "first\nsecond", 1, &match, 0) != 0 ||
        match.rm_so != 6 || match.rm_eo != 12) {
        record_fail("libctest: FAIL regex newline", 61);
    }
    regfree(&regex);
    if (regcomp(&regex, "KIWI", REG_ICASE) != 0 ||
        regexec(&regex, "kiwi os", 1, &match, 0) != 0 ||
        match.rm_so != 0 || match.rm_eo != 4) {
        record_fail("libctest: FAIL regex icase", 61);
    }
    regfree(&regex);
    if (regcomp(&regex, "unterminated[", 0) != REG_EBRACK) {
        record_fail("libctest: FAIL regex errors", 61);
    }
    fp = fopen("libctest.glob", "w");
    if (!fp || fputs("glob\n", fp) == EOF || fclose(fp) != 0) {
        record_fail("libctest: FAIL glob setup", 62);
    }
    memset(&glob_result, 0, sizeof(glob_result));
    if (glob("libctest.*", 0, NULL, &glob_result) != 0 ||
        glob_result.gl_pathc < 1 || !glob_result.gl_pathv) {
        globfree(&glob_result);
        remove("libctest.glob");
        record_fail("libctest: FAIL glob", 63);
    }
    {
        int saw_glob = 0;
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            if (strcmp(glob_result.gl_pathv[i], "libctest.glob") == 0) {
                saw_glob = 1;
            }
        }
        if (!saw_glob) {
            globfree(&glob_result);
            remove("libctest.glob");
            record_fail("libctest: FAIL glob wildcard", 64);
        }
    }
    globfree(&glob_result);
    if (mkdir("libctest.globdir", 0700) != 0) {
        remove("libctest.glob");
        record_fail("libctest: FAIL recursive glob mkdir", 101);
    }
    fp = fopen("libctest.globdir/nested.txt", "w");
    if (!fp || fputs("nested\n", fp) == EOF || fclose(fp) != 0) {
        remove("libctest.glob");
        remove("libctest.globdir/nested.txt");
        rmdir("libctest.globdir");
        record_fail("libctest: FAIL recursive glob setup", 102);
    }
    memset(&glob_result, 0, sizeof(glob_result));
    if (glob("libctest.globdir/*.txt", 0, NULL, &glob_result) != 0 ||
        glob_result.gl_pathc != 1 ||
        strcmp(glob_result.gl_pathv[0], "libctest.globdir/nested.txt") != 0) {
        globfree(&glob_result);
        remove("libctest.glob");
        remove("libctest.globdir/nested.txt");
        rmdir("libctest.globdir");
        record_fail("libctest: FAIL recursive glob", 103);
    }
    globfree(&glob_result);
    if (remove("libctest.globdir/nested.txt") != 0 || rmdir("libctest.globdir") != 0) {
        remove("libctest.glob");
        record_fail("libctest: FAIL recursive glob cleanup", 104);
    }
    if (remove("libctest.glob") != 0) {
        record_fail("libctest: FAIL glob cleanup", 65);
    }
    child = sys_spawn("/bin/hello");
    if (child < 0 || wait(&status) != (pid_t)child || status != 0) {
        record_fail("libctest: FAIL wait any", 66);
    }
    if (pipe(pipefd) != 0) {
        record_fail("libctest: FAIL pipe", 67);
    }
    if (fstat(pipefd[0], &st) != 0 || !S_ISFIFO(st.st_mode)) {
        close(pipefd[0]);
        close(pipefd[1]);
        record_fail("libctest: FAIL pipe fstat", 68);
    }
    if (write(pipefd[1], "xy", 2) != 2) {
        close(pipefd[0]);
        close(pipefd[1]);
        record_fail("libctest: FAIL pipe write", 69);
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLIN) == 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        record_fail("libctest: FAIL poll pipe", 70);
    }
    FD_ZERO(&rfds);
    FD_SET(pipefd[0], &rfds);
    if (select(pipefd[0] + 1, &rfds, NULL, NULL, NULL) != 1 || !FD_ISSET(pipefd[0], &rfds)) {
        close(pipefd[0]);
        close(pipefd[1]);
        record_fail("libctest: FAIL select pipe", 71);
    }
    memset(buf, 0, sizeof(buf));
    if (read(pipefd[0], buf, 2) != 2 || strcmp(buf, "xy") != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        record_fail("libctest: FAIL pipe read", 72);
    }
    close(pipefd[1]);
    if (read(pipefd[0], buf, 1) != 0) {
        close(pipefd[0]);
        record_fail("libctest: FAIL pipe eof", 73);
    }
    close(pipefd[0]);
    if (setupterm(NULL, STDOUT_FILENO, NULL) != 0 ||
        tgetent(NULL, "xterm") != 1 ||
        tigetstr("clear") == (char*)-1 ||
        tgetstr("cl", NULL) == NULL ||
        strcmp(tparm(tigetstr("cup"), 2L, 3L), "\x1b[3;4H") != 0 ||
        strcmp(tgoto(tgetstr("cm", NULL), 3, 2), "\x1b[3;4H") != 0) {
        record_fail("libctest: FAIL termcap", 74);
    }
    if (mbrtowc(&wc, "A", 1, NULL) != 1 || wc != L'A' ||
        wcwidth(wc) != 1 || !iswprint((wint_t)wc) || towlower((wint_t)L'Z') != (wint_t)L'z') {
        record_fail("libctest: FAIL wchar", 75);
    }
    mbsrc = "Wide";
    memset(wcbuf, 0, sizeof(wcbuf));
    if (mbsrtowcs(wcbuf, &mbsrc, 16, NULL) != 4 || mbsrc != NULL ||
        wcscmp(wcbuf, L"Wide") != 0 ||
        wcsnlen(wcbuf, 2) != 2 ||
        !wcsstr(wcbuf, L"id") ||
        wcspbrk(wcbuf, L"e") != &wcbuf[3] ||
        wcsspn(L"abc123", L"abc") != 3 ||
        wcscspn(L"abc123", L"123") != 3 ||
        !iswctype((wint_t)L'A', wctype("alpha")) ||
        towctrans((wint_t)L'A', wctrans("tolower")) != (wint_t)L'a') {
        record_fail("libctest: FAIL wchar extended", 83);
    }
    if (wcscoll(L"abc", L"abd") >= 0 ||
        wcsxfrm(wcollate_buf, L"xy", 16) != 2 ||
        wcscmp(wcollate_buf, L"xy") != 0) {
        record_fail("libctest: FAIL wide collation", 105);
    }
    wcsrc = L"MB";
    memset(mbbuf, 0, sizeof(mbbuf));
    if (wcsrtombs(mbbuf, &wcsrc, sizeof(mbbuf), NULL) != 2 || wcsrc != NULL ||
        strcmp(mbbuf, "MB") != 0) {
        record_fail("libctest: FAIL wide to multibyte", 84);
    }

    fprintf(stderr, "libctest: stderr path ok\n");
    printf("libctest: printf path %s\n", "ok");
    if (g_failure_count != 0) {
        printf("libctest: FAIL %d failure(s), first code %d\n",
               g_failure_count,
               g_first_failure_code);
        return g_first_failure_code;
    }
    puts("libctest: PASS stdio/stdlib/getopt/fd");
    return 0;
}
