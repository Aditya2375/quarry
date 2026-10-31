/* LD_PRELOAD fault-injection shim for regression tests. Intercepts
 * write/ftruncate/fsync on file descriptors whose path ends in ".wal"
 * and fails them when the matching env var is set. If QSHIM_MARKER names
 * a file, failures only start once that file exists (so a test can arm
 * the failure between statements, e.g. right before db_close). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/file.h>
#include <errno.h>
#include <stdio.h>

static int armed(void) {
    if (!getenv("QSHIM")) return 0;
    const char *m = getenv("QSHIM_MARKER");
    return !m || access(m, F_OK) == 0;
}

static int is_wal(int fd) {
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    size_t l = strlen(path);
    return l >= 4 && !strcmp(path + l - 4, ".wal");
}

ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    static ssize_t (*real)(int, void *, size_t, off_t);
    if (!real) real = dlsym(RTLD_NEXT, "pread");
    if (armed() && getenv("QSHIM_FAIL_READ") && is_wal(fd)) { errno = EIO; return -1; }
    return real(fd, buf, n, off);
}

ssize_t write(int fd, const void *buf, size_t n) {
    static ssize_t (*real)(int, const void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "write");
    if (armed() && getenv("QSHIM_FAIL_WRITE") && is_wal(fd)) { errno = EIO; return -1; }
    return real(fd, buf, n);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    static ssize_t (*real)(int, const void *, size_t, off_t);
    if (!real) real = dlsym(RTLD_NEXT, "pwrite");
    if (armed() && getenv("QSHIM_FAIL_WRITE") && is_wal(fd)) { errno = EIO; return -1; }
    return real(fd, buf, n, off);
}

int ftruncate(int fd, off_t len) {
    static int (*real)(int, off_t);
    if (!real) real = dlsym(RTLD_NEXT, "ftruncate");
    if (armed() && getenv("QSHIM_FAIL_TRUNCATE") && is_wal(fd)) { errno = EIO; return -1; }
    return real(fd, len);
}

static int is_db(int fd) {
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    size_t l = strlen(path);
    return l >= 4 && !strcmp(path + l - 4, ".qry");
}

int flock(int fd, int op) {
    static int (*real)(int, int);
    if (!real) real = dlsym(RTLD_NEXT, "flock");
    if (armed() && getenv("QSHIM_FAIL_FLOCK")) { errno = ENOLCK; return -1; }
    return real(fd, op);
}

int fsync(int fd) {
    static int (*real)(int);
    if (!real) real = dlsym(RTLD_NEXT, "fsync");
    if (armed() && getenv("QSHIM_FAIL_FSYNC") && is_wal(fd)) { errno = EIO; return -1; }
    if (armed() && getenv("QSHIM_FAIL_MAIN_FSYNC") && is_db(fd)) { errno = EIO; return -1; }
    return real(fd);
}
