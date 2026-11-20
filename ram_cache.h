#ifndef RAM_CACHE_H
#define RAM_CACHE_H

#include <string>

// =================================================================================
// ram_cache - RAM-only scratch storage for streamed audio.
//
// Downloaded track data is kept in an anonymous, unlinked file (memfd_create,
// falling back to an unlink-after-open file under /dev/shm) instead of a real
// file on /mnt/us. Both backings are tmpfs/page-cache only: the bytes never
// reach the eMMC, and they vanish the moment the fd is closed - no cleanup,
// no wear. The device's swap is zram (compressed RAM), so even under memory
// pressure this data still never touches flash.
//
// The returned "/proc/self/fd/<n>" path lets existing file-based decoders
// (mp4read, miniaudio) read it with an ordinary fopen()/fseek(), unchanged.
// =================================================================================

struct RamFile {
    int fd = -1;
    std::string path;   // "/proc/self/fd/<fd>"; valid only while fd stays open
};

// Opens a fresh RAM-backed scratch file. fd is -1 on failure (caller should
// fall back to a real file on /mnt/us in that case).
RamFile ram_open(const char* debug_name);
void ram_close(RamFile& f);

// A sensible maximum size (bytes) for one RAM-buffered track, sized from
// memory actually free right now (/proc/meminfo) so a low-memory moment falls
// back to disk instead of risking an OOM. `secondary` is for the look-ahead
// prefetch buffer, which sits alongside the one currently playing.
long long ram_track_budget(bool secondary);

// True when free memory is tight enough that RAM buffering should be skipped
// entirely in favour of the old disk-cache path.
bool ram_low_memory();

#endif // RAM_CACHE_H
