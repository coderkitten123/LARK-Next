// LD_PRELOAD shim for the host preview: redirects the device path /mnt/us/... to $LARK_PREVIEW_MNT_US
// (default /tmp/lark-next-preview/mnt_us) for the common libc file calls. system()/popen() children are NOT covered.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static const char* fix(const char* p, char* buf, size_t n) {
    if (!p || strncmp(p, "/mnt/us", 7) != 0 || (p[7] != '/' && p[7] != 0)) return p;
    const char* root = getenv("LARK_PREVIEW_MNT_US");
    if (!root || !*root) root = "/tmp/lark-next-preview/mnt_us";
    snprintf(buf, n, "%s%s", root, p + 7);
    return buf;
}
#define NEXT(name) static __typeof__(&name) real; if (!real) real = dlsym(RTLD_NEXT, #name)
#define FIX(p) char b_[4096]; p = fix(p, b_, sizeof b_)

int open(const char* p, int fl, ...) {
    NEXT(open); FIX(p); mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, fl); m = va_arg(a, mode_t); va_end(a); }
    return real(p, fl, m);
}
int open64(const char* p, int fl, ...) {
    NEXT(open64); FIX(p); mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, fl); m = va_arg(a, mode_t); va_end(a); }
    return real(p, fl, m);
}
FILE* fopen(const char* p, const char* m) { NEXT(fopen); FIX(p); return real(p, m); }
FILE* fopen64(const char* p, const char* m) { NEXT(fopen64); FIX(p); return real(p, m); }
int mkdir(const char* p, mode_t m) { NEXT(mkdir); FIX(p); return real(p, m); }
int access(const char* p, int m) { NEXT(access); FIX(p); return real(p, m); }
int stat(const char* p, struct stat* s) { NEXT(stat); FIX(p); return real(p, s); }
int lstat(const char* p, struct stat* s) { NEXT(lstat); FIX(p); return real(p, s); }
int stat64(const char* p, struct stat64* s) { NEXT(stat64); FIX(p); return real(p, s); }
int lstat64(const char* p, struct stat64* s) { NEXT(lstat64); FIX(p); return real(p, s); }
DIR* opendir(const char* p) { NEXT(opendir); FIX(p); return real(p); }
int unlink(const char* p) { NEXT(unlink); FIX(p); return real(p); }
int remove(const char* p) { NEXT(remove); FIX(p); return real(p); }
int rename(const char* a, const char* b) {
    NEXT(rename); char x[4096], y[4096]; a = fix(a, x, sizeof x); b = fix(b, y, sizeof y); return real(a, b);
}
