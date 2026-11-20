#include "ram_cache.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <atomic>

// memfd_create(2): syscall number is present in the Kindle's cross sysroot
// (kernel 4.1, well above the 3.17 minimum), but its glibc (2.20) predates
// the libc wrapper (added in 2.27). Call the syscall directly instead.
#ifndef SYS_memfd_create
#if defined(__arm__)
#define SYS_memfd_create 385
#endif
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static int try_memfd(const char* name) {
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, MFD_CLOEXEC);
#else
    (void)name;
    return -1;
#endif
}

// Fallback for kernels/libcs where memfd_create is unavailable: an ordinary
// file under /dev/shm (tmpfs), unlinked immediately so it is anonymous and
// freed the moment the fd closes - same RAM-only guarantee as memfd.
static int try_shm(const char* name) {
    static std::atomic<unsigned> counter{0};
    char path[160];
    snprintf(path, sizeof(path), "/dev/shm/lark_%s_%d_%u", name, (int)getpid(), counter++);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) unlink(path);
    return fd;
}

RamFile ram_open(const char* debug_name) {
    RamFile f;
    f.fd = try_memfd(debug_name);
    if (f.fd < 0) f.fd = try_shm(debug_name);
    if (f.fd >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "/proc/self/fd/%d", f.fd);
        f.path = buf;
    }
    return f;
}

void ram_close(RamFile& f) {
    if (f.fd >= 0) close(f.fd);
    f.fd = -1;
    f.path.clear();
}

static bool read_mem_available_kb(long long& avail_kb) {
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) return false;
    char line[256];
    long long mem_free = -1, buffers = -1, cached = -1, avail = -1;
    while (fgets(line, sizeof(line), fp)) {
        long long v;
        if (sscanf(line, "MemAvailable: %lld kB", &v) == 1) avail = v;
        else if (sscanf(line, "MemFree: %lld kB", &v) == 1) mem_free = v;
        else if (sscanf(line, "Buffers: %lld kB", &v) == 1) buffers = v;
        else if (sscanf(line, "Cached: %lld kB", &v) == 1) cached = v;
    }
    fclose(fp);
    if (avail >= 0) { avail_kb = avail; return true; }
    if (mem_free >= 0) {   // older kernels: approximate MemAvailable
        avail_kb = mem_free + (buffers > 0 ? buffers : 0) + (cached > 0 ? cached : 0);
        return true;
    }
    return false;
}

bool ram_low_memory() {
    long long avail_kb;
    if (!read_mem_available_kb(avail_kb)) return true;   // unknown: be conservative
    return avail_kb < 24 * 1024;   // under ~24MB free: too tight to risk it
}

long long ram_track_budget(bool secondary) {
    long long avail_kb;
    if (!read_mem_available_kb(avail_kb)) avail_kb = 32 * 1024;   // unknown: assume little

    // A modest, adaptive slice of what's free right now - small Kindles can
    // have well under 200MB available, so this stays conservative by default
    // and shrinks further whenever memory is already tight.
    double fraction = secondary ? 0.08 : 0.15;
    long long budget = (long long)(avail_kb * 1024.0 * fraction);

    long long min_b = 4LL * 1024 * 1024;
    long long max_b = secondary ? 24LL * 1024 * 1024 : 48LL * 1024 * 1024;
    if (budget < min_b) budget = min_b;
    if (budget > max_b) budget = max_b;
    return budget;
}
