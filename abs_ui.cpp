// =================================================================================
// abs_ui.cpp - Stage 2 (library browser UI) + Stage 3 (ABS playback glue)
//
// Flows:
//   open_abs_browser()      : config check -> library window (list, covers, search)
//   row click               : item detail -> /play session -> download track -> play
//   EOS / app close         : sync progress, chain tracks, close session
//
// Threading model: every network call (page loads, cover fetches, item details,
// session starts, track downloads, progress syncs) runs on a single background
// worker thread; results are marshalled back to the GTK main loop with
// g_idle_add. The UI thread only touches GTK + playback state -> no freezes,
// no unresponsive buttons, even when the server is slow or unreachable.
//
// Playback uses download-to-tmp for the selected track (LAN-fast, enables exact
// resume + seeking), then chains on EOS. The NEXT track is prefetched in the
// background so track changes are instant.
// =================================================================================

#include <gtk/gtk.h>
#include <glib.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include "abs_client.h"
#include "abs_ui.h"
#include "app_actions.h"
#include "database_manager.h"
#include "music_backend.h"
#include "eink_refresh.h"
#include "icons.h"
#include "ui_kit.h"
#include "ram_cache.h"

extern "C" {
#include "third_party/cJSON/cJSON.h"
}

// --- externs to m4b_player.cpp globals ---
extern MusicBackend backend;
extern DatabaseManager db;
extern GtkWidget *window;
extern bool dispUpdate;
extern bool is_hires;
extern int last_timestamp;

// Local file with no ABS context - referenced so we can clear the "current file"
// state safely. Defined in m4b_player.cpp.
extern std::string current_file;
std::string get_display_name(const std::string& path);

AbsClient gAbs;

// ---------------------------------------------------------------------------------
// ABS playback state (ONLY touched from the GTK main thread)
// ---------------------------------------------------------------------------------

// Track cache: used only as a fallback when a track can't be RAM-buffered
// (format mp4read/miniaudio can't stream-detect, or the track is too big for
// the current RAM budget - see ram_cache.h). The normal case never touches
// this directory at all: tracks are downloaded into an anonymous RAM-backed
// fd (ram_cache.h) and played from "/proc/self/fd/N", so the eMMC never sees
// repeat multi-MB audio writes.
static const char* ABS_CACHE_DIR = "/mnt/us/abs_cache";
static const char* ABS_COVER_DIR = "/mnt/us/abs_covers";
static const char* ABS_DOWNLOAD_EXT = ".abs.json";

struct AbsDownload {
    AbsItem item;
    std::vector<AbsChapter> chapters;
    std::vector<AbsTrack> tracks;
};

static bool abs_safe_item_id(const std::string& id) {
    if (id.empty()) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static std::string abs_download_manifest_path(const std::string& id) {
    if (!abs_safe_item_id(id)) return "";
    return std::string(ABS_CACHE_DIR) + "/" + id + ABS_DOWNLOAD_EXT;
}

static std::string abs_download_track_path(const std::string& id, int index) {
    if (!abs_safe_item_id(id) || index <= 0) return "";
    char path[320];
    snprintf(path, sizeof(path), "%s/%s_%d.mp3", ABS_CACHE_DIR, id.c_str(), index);
    return path;
}

static bool abs_download_space_budget(long long& max_bytes) {
    struct statvfs space;
    if (statvfs(ABS_CACHE_DIR, &space) != 0) return false;
    unsigned long long available = (unsigned long long)space.f_bavail *
                                  (unsigned long long)space.f_frsize;
    const unsigned long long reserve = 16ULL * 1024ULL * 1024ULL;
    if (available <= reserve) return false;
    max_bytes = (long long)(available - reserve);
    return true;
}

static bool abs_save_download_manifest(const AbsDownload& d) {
    std::string path = abs_download_manifest_path(d.item.id);
    if (path.empty()) return false;
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "id", d.item.id.c_str());
    cJSON_AddStringToObject(root, "title", d.item.title.c_str());
    cJSON_AddStringToObject(root, "author", d.item.author.c_str());
    cJSON_AddNumberToObject(root, "duration", d.item.duration);
    cJSON* chapters = cJSON_CreateArray();
    for (size_t i = 0; i < d.chapters.size(); ++i) {
        cJSON* ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "start", d.chapters[i].start);
        cJSON_AddNumberToObject(ch, "end", d.chapters[i].end);
        cJSON_AddStringToObject(ch, "title", d.chapters[i].title.c_str());
        cJSON_AddItemToArray(chapters, ch);
    }
    cJSON_AddItemToObject(root, "chapters", chapters);
    cJSON* tracks = cJSON_CreateArray();
    for (size_t i = 0; i < d.tracks.size(); ++i) {
        cJSON* tr = cJSON_CreateObject();
        cJSON_AddNumberToObject(tr, "index", d.tracks[i].index);
        cJSON_AddNumberToObject(tr, "start", d.tracks[i].start_offset);
        cJSON_AddNumberToObject(tr, "duration", d.tracks[i].duration);
        cJSON_AddStringToObject(tr, "mime", d.tracks[i].mime_type.c_str());
        cJSON_AddItemToArray(tracks, tr);
    }
    cJSON_AddItemToObject(root, "tracks", tracks);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    bool ok = f && fwrite(json, 1, strlen(json), f) == strlen(json);
    if (f && fclose(f) != 0) ok = false;
    free(json);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

static std::string abs_json_string(const cJSON* obj, const char* key, const char* fallback = "") {
    cJSON* node = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return cJSON_IsString(node) && node->valuestring ? node->valuestring : fallback;
}

static double abs_json_number(const cJSON* obj, const char* key, double fallback = 0.0) {
    cJSON* node = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return cJSON_IsNumber(node) ? node->valuedouble : fallback;
}

static bool abs_load_download_manifest(const std::string& path, AbsDownload& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return false;
    }
    std::vector<char> bytes((size_t)size + 1);
    size_t got = fread(&bytes[0], 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return false;
    bytes[got] = '\0';
    cJSON* root = cJSON_Parse(&bytes[0]);
    if (!root) return false;
    AbsDownload d;
    d.item.id = abs_json_string(root, "id");
    d.item.title = abs_json_string(root, "title");
    d.item.author = abs_json_string(root, "author");
    d.item.duration = abs_json_number(root, "duration");
    d.item.progress = 0;
    d.item.finished = false;
    d.item.num_tracks = 0;
    d.item.num_chapters = 0;
    cJSON* chapters = cJSON_GetObjectItemCaseSensitive(root, "chapters");
    cJSON* node;
    cJSON_ArrayForEach(node, chapters) {
        AbsChapter ch;
        ch.start = abs_json_number(node, "start");
        ch.end = abs_json_number(node, "end");
        ch.title = abs_json_string(node, "title");
        d.chapters.push_back(ch);
    }
    cJSON* tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    cJSON_ArrayForEach(node, tracks) {
        AbsTrack tr;
        tr.index = (int)abs_json_number(node, "index");
        tr.start_offset = abs_json_number(node, "start");
        tr.duration = abs_json_number(node, "duration");
        tr.mime_type = abs_json_string(node, "mime", "audio/mpeg");
        if (tr.index <= 0 || abs_download_track_path(d.item.id, tr.index).empty()) continue;
        struct stat st;
        if (stat(abs_download_track_path(d.item.id, tr.index).c_str(), &st) != 0 || st.st_size <= 0) {
            cJSON_Delete(root);
            return false;
        }
        tr.content_url = abs_download_track_path(d.item.id, tr.index);
        d.tracks.push_back(tr);
    }
    cJSON_Delete(root);
    if (!abs_safe_item_id(d.item.id) || d.tracks.empty()) return false;
    d.item.num_tracks = (int)d.tracks.size();
    d.item.num_chapters = (int)d.chapters.size();
    out = d;
    return true;
}

static std::vector<AbsDownload> abs_list_downloads() {
    std::vector<AbsDownload> out;
    DIR* dir = opendir(ABS_CACHE_DIR);
    if (!dir) return out;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        std::string name = ent->d_name;
        size_t ext = name.size() >= strlen(ABS_DOWNLOAD_EXT) ? name.size() - strlen(ABS_DOWNLOAD_EXT) : std::string::npos;
        if (ext == std::string::npos || name.compare(ext, std::string::npos, ABS_DOWNLOAD_EXT) != 0) continue;
        AbsDownload d;
        if (abs_load_download_manifest(std::string(ABS_CACHE_DIR) + "/" + name, d)) {
            char key[256];
            snprintf(key, sizeof(key), "abs_download_pos_%s", d.item.id.c_str());
            d.item.progress = atof(db.getSetting(key, "0").c_str());
            out.push_back(d);
        }
    }
    closedir(dir);
    return out;
}

static void abs_remove_download(const AbsDownload& d) {
    for (size_t i = 0; i < d.tracks.size(); ++i)
        remove(abs_download_track_path(d.item.id, d.tracks[i].index).c_str());
    remove(abs_download_manifest_path(d.item.id).c_str());
    char key[256];
    snprintf(key, sizeof(key), "abs_download_pos_%s", d.item.id.c_str());
    db.setSetting(key, "");
}
// Browse page size is set per view mode (cover grid vs list); see the
// browser section. Small pages keep list/grid building instant — laying out
// hundreds of GTK rows in one go was a major source of UI lag on the Kindle.
static const int   ABS_SYNC_INTERVAL = 15;   // ticks (1s each) between /sync calls

struct AbsPlayback {
    bool active = false;
    bool offline = false;
    std::string session_id;
    std::string item_id;
    std::string book_title;
    std::string book_author;
    std::string busy_msg;               // shown on the chapter line instead of the chapter
                                        // while a book/track is loading
    std::vector<AbsTrack> tracks;
    std::vector<AbsChapter> chapters;    // server chapters (time display + chapter list)
    int track_idx = -1;                 // current track (0-based into tracks)
    double book_position = 0;           // seconds into the book
    double book_duration = 0;
    double last_sync_position = 0;      // book position at last /sync
    int last_sync_tick = 0;             // updated once per abs_sync_tick call

    // RAM-backed track buffers (ram_cache.h). -1 = none / disk-backed instead.
    int cur_fd = -1;        // backs the currently loaded track's "/proc/self/fd/N" path
    int cur_fd_idx = -1;    // which track index cur_fd belongs to
    int next_fd = -1;       // look-ahead buffer for the next track, downloaded early
    int next_fd_idx = -1;
};
static AbsPlayback ap;

// ABS playback takes over the shared player controls. Save any local book's
// current position first, then clear its identity so controls after an ABS
// book finishes can't unexpectedly restart the previous local file.
static void abs_leave_local_file() {
    if (!current_file.empty() && !ap.active &&
        (backend.is_playing || backend.is_paused)) {
        int pos = (int)(backend.get_position() / GST_SECOND);
        int dur = (int)(backend.get_duration() / GST_SECOND);
        db.updateBookProgress(current_file, pos, dur, false);
    }
    current_file.clear();
    last_timestamp = 0;
}

static void abs_store_offline_position(double position) {
    if (!ap.offline || ap.item_id.empty()) return;
    char key[256], value[48];
    snprintf(key, sizeof(key), "abs_download_pos_%s", ap.item_id.c_str());
    snprintf(value, sizeof(value), "%.0f", position < 0 ? 0 : position);
    db.setSetting(key, value);
}

// Guesses the local decoder format from an Audiobookshelf track's MIME type,
// so a RAM-backed "/proc/self/fd/N" path (no file extension to sniff) still
// decodes correctly. UNKNOWN falls back to the disk cache, where the real
// filename's extension drives the existing detection.
static AudioFormat abs_format_for_mime(const std::string& mime) {
    if (mime.find("mp4") != std::string::npos || mime.find("m4b") != std::string::npos ||
        mime.find("m4a") != std::string::npos || mime.find("aac") != std::string::npos)
        return AudioFormat::M4B_AAC;
    if (mime.find("mpeg") != std::string::npos || mime.find("mp3") != std::string::npos ||
        mime.find("flac") != std::string::npos || mime.find("wav") != std::string::npos ||
        mime.find("ogg") != std::string::npos)
        return AudioFormat::MINIAUDIO;
    return AudioFormat::UNKNOWN;
}

static void abs_drop_cur_ram_fd() {
    if (ap.cur_fd >= 0) close(ap.cur_fd);
    ap.cur_fd = -1;
    ap.cur_fd_idx = -1;
}

static void abs_drop_next_ram_fd() {
    if (ap.next_fd >= 0) close(ap.next_fd);
    ap.next_fd = -1;
    ap.next_fd_idx = -1;
}

// Called whenever the book session fully ends (finished / switched books /
// local file opened / app exit) so no RAM stays reserved past that point.
static void abs_close_ram_fds() {
    abs_drop_cur_ram_fd();
    abs_drop_next_ram_fd();
}

// Takes ownership of a newly-downloaded RAM fd for track `idx`, closing
// whatever the previous one was (a different track no longer needed).
static void abs_adopt_ram_fd(int fd, int idx) {
    if (ap.cur_fd >= 0 && ap.cur_fd != fd) close(ap.cur_fd);
    ap.cur_fd = fd;
    ap.cur_fd_idx = idx;
}

bool abs_is_active() { return ap.active; }

// Backend EOS callback adapter (EosCallback signature)
void abs_on_eos_wrapper(void* user_data) {
    (void)user_data;
    abs_on_eos();
}

// ---------------------------------------------------------------------------------
// Background worker: all AbsClient calls run here, UI via g_idle_add
// ---------------------------------------------------------------------------------

namespace {

struct Job {
    std::function<void()>* fn;   // heap-allocated: g_new memory can't hold a
                                 // std::function directly (uninitialized assign = UB)
};
struct IdleFn {
    std::function<void()> fn;
};

GAsyncQueue* g_jobq = nullptr;

gpointer worker_main(gpointer) {
    for (;;) {
        Job* j = (Job*)g_async_queue_pop(g_jobq);
        (*j->fn)();
        delete j->fn;
        g_free(j);
    }
    return NULL;
}

// Run fn on the worker thread.
void abs_async(std::function<void()> fn) {
    if (!g_jobq) {
        g_jobq = g_async_queue_new();
        // Kindle firmware glib is 2.29: g_thread_create_full only (threads
        // are always initialized at this version, no g_thread_init needed)
        g_thread_create_full(worker_main, NULL, 0, FALSE, TRUE,
                             G_THREAD_PRIORITY_NORMAL, NULL);
    }
    Job* j = g_new(Job, 1);
    j->fn = new std::function<void()>(std::move(fn));
    g_async_queue_push(g_jobq, j);
}

void idle_destroy(gpointer p) { delete (IdleFn*)p; }
gboolean idle_trampoline(gpointer p) {
    ((IdleFn*)p)->fn();
    return FALSE;   // one-shot
}

// Run fn once on the GTK main thread.
void abs_idle(std::function<void()> fn) {
    IdleFn* j = new IdleFn();
    j->fn = std::move(fn);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, idle_trampoline, j, idle_destroy);
}

} // namespace

// ---------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------

static std::string fmt_time(double secs) {
    if (secs < 0) secs = 0;
    int s = (int)secs;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
    return buf;
}

// Short display string for row subtitles / chapter labels
static std::string trunc_str(const std::string& s, size_t maxlen) {
    if (s.size() <= maxlen) return s;
    return s.substr(0, maxlen - 3) + "...";
}

// Compact human duration: "9h 12m" / "45m"
static std::string fmt_hm(double secs) {
    int s = (int)(secs + 0.5);
    if (s < 0) s = 0;
    char buf[32];
    if (s >= 3600) {
        int h = s / 3600, m = (s % 3600) / 60;
        if (m) snprintf(buf, sizeof(buf), "%dh %dm", h, m);
        else   snprintf(buf, sizeof(buf), "%dh", h);
    } else {
        snprintf(buf, sizeof(buf), "%dm", (s + 59) / 60);
    }
    return buf;
}

// One-line progress info for list rows, grid cells and the book detail card
static std::string abs_progress_text(const AbsItem& it) {
    if (it.finished) return "Finished";
    if (it.duration > 0 && it.progress > 0) {
        int pct = (int)(it.progress * 100 / it.duration);
        if (pct < 1) pct = 1;
        if (pct > 99) pct = 99;
        double left = it.duration > it.progress ? it.duration - it.progress : 0;
        return std::to_string(pct) + "% - " + fmt_hm(left) + " left";
    }
    if (it.duration > 0) return fmt_hm(it.duration);
    return "";
}

// ---------------------------------------------------------------------------------
// Session persistence: so a crash/kill mid-book doesn't strand an un-synced
// session. Every successful sync stamps these settings; a clean close clears
// them. abs_reconcile_stale_session() (called at startup) finalizes anything
// left behind by an ungraceful exit, so other Audiobookshelf clients see the
// real last position instead of a stale one from whenever we last synced.
// ---------------------------------------------------------------------------------

static void abs_remember_session(const std::string& sid, const std::string& item_id,
                                 double pos, double dur) {
    db.setSetting("abs_last_session_id", sid);
    db.setSetting("abs_last_session_item", item_id);
    db.setSetting("abs_last_session_pos", std::to_string((long long)pos));
    db.setSetting("abs_last_session_dur", std::to_string((long long)dur));
}

static void abs_forget_session() {
    db.setSetting("abs_last_session_id", "");
    db.setSetting("abs_last_session_item", "");
}

static void abs_forget_session_if_matches(const std::string& sid) {
    if (db.getSetting("abs_last_session_id", "") == sid)
        abs_forget_session();
}

static std::string abs_norm_server(std::string s);

static void show_abs_error(const std::string& msg) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(window),
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                          "%s", msg.c_str());
    gtk_window_set_title(GTK_WINDOW(d), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

// Chapter containing a book position (or NULL)
static const AbsChapter* abs_chapter_at(double book_pos) {
    for (size_t i = 0; i < ap.chapters.size(); i++) {
        if (book_pos >= ap.chapters[i].start && book_pos < ap.chapters[i].end)
            return &ap.chapters[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------------
// Cover loading (worker thread -> idle apply)
// ---------------------------------------------------------------------------------

static bool abs_is_jpeg_file(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	unsigned char magic[2] = { 0, 0 };
	bool ok = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
		magic[0] == 0xFF && magic[1] == 0xD8;
	fclose(f);
	return ok;
}

static void abs_load_cover(const std::string& item_id) {
    std::string path = std::string(ABS_COVER_DIR) + "/" + item_id + ".jpg";
    abs_async([item_id, path] {
        if (!abs_is_jpeg_file(path)) {
            remove(path.c_str());
            mkdir(ABS_COVER_DIR, 0777);
            if (!gAbs.getCover(item_id, 400, path) || !abs_is_jpeg_file(path))
                remove(path.c_str());
        }
        abs_idle([item_id, path] {
            if (!ap.active || ap.item_id != item_id) return;   // another book meanwhile
            GdkPixbuf* pix = gdk_pixbuf_new_from_file(path.c_str(), NULL);
            if (pix) {
                player_set_cover(pix);
                g_object_unref(pix);
            }
            // else: leave the previous cover / icon (e-ink friendly)
        });
    });
}

// ---------------------------------------------------------------------------------
// Playback glue (track start machinery, EOS chaining, sync)
// ---------------------------------------------------------------------------------

struct TrackStart {
    guint    gen;
    int      idx;
    double   offset;
    std::string path;
};
static guint g_track_gen = 0;   // only the newest track start may play

static void abs_begin_track(int idx, double offset_in_track);

static void abs_play_now(int idx, double offset_in_track, const std::string& path,
                         AudioFormat forced_format) {
    backend.stop();
    backend.play_file(path.c_str(), (int)offset_in_track, forced_format);

    ap.busy_msg.clear();   // was "Downloading part N..."

    // Prefetch the next track in the background so EOS chaining is instant.
    // RAM first (bounded by ram_track_budget), falling back to the disk cache
    // only when the format can't be stream-detected or memory is tight.
    int next = idx + 1;
    if (!ap.offline && next < (int)ap.tracks.size() && ap.next_fd < 0) {
        const AbsTrack& nt = ap.tracks[next];
        AudioFormat nforced = abs_format_for_mime(nt.mime_type);
        std::string url = nt.content_url;
        char dpath[256];
        snprintf(dpath, sizeof(dpath), "%s/%s_%d.mp3", ABS_CACHE_DIR, ap.item_id.c_str(), nt.index);
        std::string disk_path = dpath;
        bool try_ram = nforced != AudioFormat::UNKNOWN && !ram_low_memory();
        long long budget = ram_track_budget(true);
        guint pgen = g_track_gen;   // only adopted if still relevant once it lands

        abs_async([pgen, next, url, disk_path, try_ram, budget] {
            if (try_ram) {
                RamFile rf = ram_open("abs_next");
                if (rf.fd >= 0) {
                    long long written = 0;
                    bool ok = gAbs.downloadToFd(url, rf.fd, budget, written);
                    if (ok) {
                        int fd = rf.fd;
                        abs_idle([pgen, next, fd] {
                            if (pgen != g_track_gen || !ap.active || ap.next_fd >= 0) {
                                close(fd);   // stale, or another prefetch already landed
                                return;
                            }
                            ap.next_fd = fd;
                            ap.next_fd_idx = next;
                        });
                        return;
                    }
                    close(rf.fd);   // too big for the budget, or a transient failure
                }
            }
            // RAM not attempted or it failed: old disk-prefetch behaviour.
            struct stat st;
            if (stat(disk_path.c_str(), &st) != 0 || st.st_size == 0) {
                mkdir(ABS_CACHE_DIR, 0777);
                gAbs.downloadFile(url, disk_path);
            }
        });
    }

    // Update the time labels before asking the e-ink driver to repaint. A
    // direct ioctl here raced GTK's queued label redraw on Kindle.
    abs_time_display(dispUpdate);
    app_schedule_refresh(false);
}

static void abs_begin_track(int idx, double offset_in_track) {
    if (!ap.active || idx < 0 || idx >= (int)ap.tracks.size()) return;
    // The requested track may need a network download before playback can
    // restart. Stop the previous decoder now so its position/EOS cannot be
    // attributed to the newly selected track while that download is pending.
    backend.stop();
    const AbsTrack& tr = ap.tracks[idx];
    if (offset_in_track < 0) offset_in_track = 0;
    if (tr.duration > 0 && offset_in_track > tr.duration) offset_in_track = tr.duration;

    ap.track_idx = idx;
    ap.book_position = tr.start_offset + offset_in_track;
    ap.last_sync_position = ap.book_position;
    ap.last_sync_tick = 0;

    player_set_book(ap.book_title, ap.book_author);
    player_apply_display_mode();
    if (!ap.offline) abs_load_cover(ap.item_id);

    guint gen = ++g_track_gen;
    AudioFormat forced = abs_format_for_mime(tr.mime_type);

    // 1) Same track already loaded in RAM (seeking within it, e.g. the sleep
    //    timer, a chapter jump, or fast-forward/rewind): just re-point
    //    playback at the existing buffer. No network, no disk, instant.
    if (ap.cur_fd >= 0 && ap.cur_fd_idx == idx) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", ap.cur_fd);
        abs_play_now(idx, offset_in_track, path, forced);
        return;
    }

    // 2) Already sitting in the look-ahead RAM buffer (normal EOS chaining
    //    onto the next track): adopt it, again no network/disk.
    if (ap.next_fd >= 0 && ap.next_fd_idx == idx) {
        int fd = ap.next_fd;
        ap.next_fd = -1;
        ap.next_fd_idx = -1;
        abs_adopt_ram_fd(fd, idx);
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        abs_play_now(idx, offset_in_track, path, forced);
        return;
    }

    // 3) A previous run (or the oversized/unrecognised-format fallback) left
    //    this track cached on disk: play it straight from there.
    char path[256];
    snprintf(path, sizeof(path), "%s/%s_%d.mp3", ABS_CACHE_DIR, ap.item_id.c_str(), tr.index);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        abs_drop_cur_ram_fd();   // switching to a disk-backed track
        abs_play_now(idx, offset_in_track, path, AudioFormat::UNKNOWN);
        return;
    }

    // 4) Fetch from the server. RAM first (bounded, never touches the eMMC);
    //    fall back to the disk cache only if the format can't be detected
    //    from the MIME type, memory is tight, or the track is too big for
    //    the current RAM budget.
    ap.busy_msg = "Downloading part " + std::to_string(idx + 1) + "...";
    player_set_info(ap.busy_msg, "", "");
    std::string url = tr.content_url;
    std::string disk_path = path;
    bool try_ram = forced != AudioFormat::UNKNOWN && !ram_low_memory();
    long long budget = ram_track_budget(false);

    abs_async([gen, idx, offset_in_track, url, disk_path, try_ram, budget, forced] {
        int fd = -1;
        bool ok = false;
        if (try_ram) {
            RamFile rf = ram_open("abs_cur");
            if (rf.fd >= 0) {
                long long written = 0;
                ok = gAbs.downloadToFd(url, rf.fd, budget, written);
                if (ok) {
                    fd = rf.fd;
                } else {
                    close(rf.fd);
                    g_print("ABS: track %d too large for the %.0fMB RAM budget "
                            "(or a download error) - caching to disk instead\n",
                            idx + 1, budget / 1024.0 / 1024.0);
                }
            }
        }
        if (!ok) {
            mkdir(ABS_CACHE_DIR, 0777);
            ok = gAbs.downloadFile(url, disk_path);
            if (!ok) remove(disk_path.c_str());
        }
        abs_idle([gen, idx, offset_in_track, disk_path, fd, ok, forced] {
            if (gen != g_track_gen || !ap.active) {   // stale start: the user moved on
                if (fd >= 0) close(fd);
                return;
            }
            if (!ok) {
                show_abs_error("Failed to download track.\nIs Wi-Fi on and server reachable?");
                return;
            }
            if (fd >= 0) {
                abs_adopt_ram_fd(fd, idx);
                char p[64];
                snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
                abs_play_now(idx, offset_in_track, p, forced);
            } else {
                abs_drop_cur_ram_fd();
                abs_play_now(idx, offset_in_track, disk_path, AudioFormat::UNKNOWN);
            }
        });
    });
}

void abs_on_eos() {
    if (!ap.active) return;   // local file EOS: no-op (upstream behaviour)
    // Position at end of current track
    if (ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size()) {
        ap.book_position = ap.tracks[ap.track_idx].start_offset + ap.tracks[ap.track_idx].duration;
    }

    int next = ap.track_idx + 1;
    if (next < (int)ap.tracks.size()) {
        g_print("ABS: track finished, chaining to track %d\n", next + 1);
        abs_begin_track(next, 0.0);
    } else {
        // Book finished
        g_print("ABS: book finished\n");
        double dur = ap.book_duration > 0 ? ap.book_duration : ap.book_position;
        if (ap.offline) {
            abs_store_offline_position(dur);
            ap.track_idx = -1;
            ap.active = false;
            ap.book_position = dur;
            ap.busy_msg.clear();
            abs_close_ram_fds();
            player_set_info("Finished", "", "");
            app_schedule_refresh(false);
            return;
        }
        std::string iid = ap.item_id;
        std::string sid = ap.session_id;
        double delta = dur - ap.last_sync_position;
        if (delta < 0) delta = 0;
        abs_remember_session(sid, iid, dur, dur);
        abs_async([iid, sid, dur, delta] {
            bool synced = gAbs.syncSession(sid, dur, delta, dur);
            bool closed = gAbs.closeSession(sid);
            bool progress_saved = gAbs.setItemProgress(iid, dur, dur);
            if (synced && closed && progress_saved)
                abs_idle([sid] { abs_forget_session_if_matches(sid); });
        });

        ap.session_id.clear();
        ap.track_idx = -1;
        ap.active = false;
        ap.book_position = 0;
        ap.busy_msg.clear();
        abs_close_ram_fds();
        abs_forget_session();
        player_set_info("Finished", "", "");
        player_set_cover(NULL);
        eink_full_refresh();
    }
}

void abs_sync_tick() {
    if (!ap.active || !backend.is_playing) return;

    double pos = backend.get_position() / GST_SECOND;
    if (ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size())
        ap.book_position = ap.tracks[ap.track_idx].start_offset + pos;

    ap.last_sync_tick++;
    if (ap.last_sync_tick < ABS_SYNC_INTERVAL) return;
    ap.last_sync_tick = 0;
    abs_force_sync();
}

// Flushes the current position to the server right now, bypassing the
// periodic throttle. Called on pause, on leaving the Player tab, when the
// app loses focus, and every ABS_SYNC_INTERVAL ticks - so another
// Audiobookshelf client (or this same book reopened later) sees a position
// that is at most a few seconds stale, not up to a whole tick interval old.
void abs_force_sync() {
    if (!ap.active) return;

    double cur = ap.book_position;
    if (backend.is_playing && ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size()) {
        cur = ap.tracks[ap.track_idx].start_offset + backend.get_position() / (double)GST_SECOND;
        ap.book_position = cur;
    }
    double delta = cur - ap.last_sync_position;
    if (delta < 0) delta = 0;   // seeked backwards
    double dur = ap.book_duration > 0 ? ap.book_duration : 0;
    if (ap.offline) {
        abs_store_offline_position(cur);
        ap.last_sync_position = cur;
        ap.last_sync_tick = 0;
        return;
    }
    if (ap.session_id.empty()) return;
    std::string sid = ap.session_id;
    std::string item = ap.item_id;

    abs_remember_session(sid, item, cur, dur);   // survives a crash/kill until closed cleanly
    abs_async([sid, cur, delta, dur] {
        if (gAbs.syncSession(sid, cur, delta, dur))
            g_print("ABS: synced progress %.0fs\n", cur);
        else
            g_print("ABS: sync failed (server unreachable?)\n");
    });
    ap.last_sync_position = cur;   // optimistic: next delta counts from now
    ap.last_sync_tick = 0;
}

// Finalizes a session left open by an earlier crash/kill (abs_shutdown() /
// book-finished / app-exit all clear the persisted keys on a clean close, so
// anything still present here means the app died mid-book). Run once at
// startup so the server's record of this book stops at the real last
// position the moment the app runs again, rather than whenever it was last
// able to sync. Uses its own AbsClient so it doesn't disturb gAbs's config.
void abs_reconcile_stale_session() {
    std::string sid = db.getSetting("abs_last_session_id", "");
    if (sid.empty()) return;

    std::string server = abs_norm_server(db.getSetting("abs_server", ""));
    if (server.empty()) {
        g_print("ABS: cannot recover the last session without a server address; keeping its marker\n");
        return;
    }

    std::string item = db.getSetting("abs_last_session_item", "");
    double pos = atof(db.getSetting("abs_last_session_pos", "0").c_str());
    double dur = atof(db.getSetting("abs_last_session_dur", "0").c_str());
    std::string token = db.getSetting("abs_token", "");
    std::string user = db.getSetting("abs_user", "");
    std::string pass = db.getSetting("abs_pass", "");

    g_print("ABS: finalizing a session left open by the last run (%.0fs)\n", pos);
    abs_async([server, token, user, pass, sid, item, pos, dur] {
        AbsClient tmp;
        tmp.setServer(server, token);
        if (token.empty()) tmp.setCredentials(user, pass);
        tmp.setHttpTimeout(8);
        tmp.syncSession(sid, pos, 0, dur);
        bool closed = tmp.closeSession(sid);
        bool progress_saved = item.empty() || tmp.setItemProgress(item, pos, dur);
        if (closed && progress_saved) {
            abs_idle([sid] { abs_forget_session_if_matches(sid); });
        } else {
            g_print("ABS: stale session recovery incomplete; will retry next launch\n");
        }
    });
}

void abs_jump_relative(int seconds) {
    if (!ap.active || ap.track_idx < 0) return;
    double pos = backend.get_position() / GST_SECOND;
    double new_track_pos = pos + seconds;
    if (new_track_pos < 0) {
        // Jump into the previous track
        int prev = ap.track_idx - 1;
        if (prev < 0) { abs_begin_track(0, 0); return; }
        double remain = -new_track_pos;
        const AbsTrack& pt = ap.tracks[prev];
        abs_begin_track(prev, pt.duration - remain > 0 ? pt.duration - remain : 0);
    } else if (ap.track_idx < (int)ap.tracks.size() &&
               new_track_pos > ap.tracks[ap.track_idx].duration) {
        int next = ap.track_idx + 1;
        if (next >= (int)ap.tracks.size()) { abs_on_eos(); return; }
        abs_begin_track(next, new_track_pos - ap.tracks[ap.track_idx].duration);
    } else {
        abs_begin_track(ap.track_idx, new_track_pos);
    }
}

// Async session finalization (worker thread); safe snapshots from main thread
static void abs_finalize_session(const std::string& sid, double cur,
                                 double last_synced, double dur, bool close_it) {
    if (sid.empty()) return;
    double delta = cur - last_synced;
    if (delta < 0) delta = 0;
    abs_async([sid, cur, delta, dur, close_it] {
        bool synced = gAbs.syncSession(sid, cur, delta, dur);
        bool closed = !close_it || gAbs.closeSession(sid);
        if (synced && closed && close_it)
            abs_idle([sid] { abs_forget_session_if_matches(sid); });
    });
}

void abs_shutdown() {
    if (!ap.active) return;
    if (backend.is_playing && ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size())
        ap.book_position = ap.tracks[ap.track_idx].start_offset + backend.get_position() / (double)GST_SECOND;
    if (ap.offline) abs_store_offline_position(ap.book_position);
    else abs_finalize_session(ap.session_id, ap.book_position, ap.last_sync_position,
                              ap.book_duration > 0 ? ap.book_duration : 0, true);
    g_print("ABS: session finalized at %.0fs\n", ap.book_position);
    ap.session_id.clear();
    ap.active = false;
    ap.offline = false;
    abs_close_ram_fds();
}

// ---- App-exit final sync (responsive close) --------------------------------
// The old abs_shutdown_sync() ran on the UI thread: with the server
// unreachable, connect() can block for tens of seconds (socket timeouts only
// apply to established connections), which made the X button look dead.
// Now: snapshot the session on the UI thread (abs_begin_shutdown_sync), run
// the sync on a detached thread, and let the app exit after a bounded wait
// (abs_wait_shutdown_sync). Close always responds instantly.
// ----------------------------------------------------------------------------

struct FinalSyncArgs {
    std::string sid;
    std::string server, token, user, pass;
    double cur, last_synced, dur;
};
static FinalSyncArgs*    g_final_args = NULL;
static volatile gboolean g_final_done = TRUE;

// NOTE: the Kindle's glib 2.29 does not export g_mutex_*/g_cond_* (only the
// older g_static_mutex_* API), so the handoff uses a single once-written
// boolean flag instead - written once by the sync thread at completion,
// polled with a bounded timeout on the main thread. Good enough by design:
// the process exits right after the wait, whatever the outcome.

static gpointer abs_final_sync_thread(gpointer) {
    FinalSyncArgs* a = g_final_args;
    if (a) {
        double delta = a->cur - a->last_synced;
        if (delta < 0) delta = 0;
        AbsClient client;   // separate from the background worker's gAbs
        client.setServer(a->server, a->token);
        if (a->token.empty()) client.setCredentials(a->user, a->pass);
        client.setHttpTimeout(3);   // short: never hold the app exit hostage
        bool synced = client.syncSession(a->sid, a->cur, delta, a->dur);
        bool closed = client.closeSession(a->sid);
        g_print("ABS: session %s at %.0fs\n",
                (synced && closed) ? "closed" : "final sync incomplete", a->cur);
        delete a;
        g_final_args = NULL;
    }
    g_final_done = TRUE;
    return NULL;
}

void abs_begin_shutdown_sync() {
    g_final_args = NULL;
    if (ap.active && ap.offline) {
        if (backend.is_playing && ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size())
            ap.book_position = ap.tracks[ap.track_idx].start_offset + backend.get_position() / (double)GST_SECOND;
        abs_store_offline_position(ap.book_position);
        ap.active = false;
        g_final_done = TRUE;
        return;
    }
    if (ap.active && ap.session_id.size()) {
        FinalSyncArgs* a = new FinalSyncArgs();
        a->sid         = ap.session_id;
        a->server      = abs_norm_server(db.getSetting("abs_server", ""));
        a->token       = db.getSetting("abs_token", "");
        a->user        = db.getSetting("abs_user", "");
        a->pass        = db.getSetting("abs_pass", "");
        a->cur         = ap.book_position;
        if (backend.is_playing && ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size())
            a->cur = ap.tracks[ap.track_idx].start_offset +
                     backend.get_position() / (double)GST_SECOND;
        ap.book_position = a->cur;
        abs_remember_session(ap.session_id, ap.item_id, a->cur,
                             ap.book_duration > 0 ? ap.book_duration : 0);
        a->last_synced = ap.last_sync_position;
        a->dur         = ap.book_duration > 0 ? ap.book_duration : 0;
        g_final_args = a;
        g_print("ABS: closing session at %.0fs\n", a->cur);
    }
    ap.active = false;
    g_final_done = g_final_args ? FALSE : TRUE;
    if (g_final_args)
        g_thread_create_full(abs_final_sync_thread, NULL, 0, FALSE, TRUE,
                             G_THREAD_PRIORITY_NORMAL, NULL);
}

void abs_wait_shutdown_sync(int timeout_ms) {
    if (g_final_done) return;
    // Bounded wait: poll the completion flag; never let close hang.
    for (int waited = 0; waited < timeout_ms && !g_final_done; waited += 50)
        usleep(50 * 1000);
}

void abs_replay_current() {
    if (!ap.active || ap.track_idx < 0) return;
    double offset = ap.book_position - ap.tracks[ap.track_idx].start_offset;
    if (offset < 0) offset = 0;
    abs_begin_track(ap.track_idx, offset);
}

// Start playback of a library item (from the browser). Fully async.
struct PlayResult {
    bool ok = false;
    std::string err;
    std::string item_id;
    std::string title;
    std::string author;
    std::string session_id;
    std::vector<AbsChapter> chapters;
    std::vector<AbsTrack> tracks;
    double current = 0;
    double udur = 0;
};

static guint g_load_gen = 0;   // only the newest book selection may start

static void abs_play_item(const std::string& item_id, const std::string& title,
                          const std::string& author) {
    abs_leave_local_file();
    if (ap.active) abs_shutdown();   // finalize previous book (async)
    backend.stop();
    g_load_gen++;                    // cancel any in-flight book load
    guint lgen = g_load_gen;

    // Immediate feedback while the worker fetches details
    player_set_book(title, author);
    player_set_cover(NULL);
    player_apply_display_mode();
    player_set_info("Loading book...", "", "");

    abs_async([item_id, title, author, lgen] {
        PlayResult* r = new PlayResult();
        r->item_id = item_id;
        r->title = title;
        r->author = author;

        if (!gAbs.getItemDetail(item_id, r->chapters, r->tracks, r->current, r->udur)) {
            r->err = "Failed to load book details.\nIs Wi-Fi on and server reachable?";
        } else if (r->tracks.empty()) {
            r->err = "This book has no playable tracks.";
        } else if (!gAbs.startPlayback(item_id, r->session_id, r->tracks) ||
                   r->session_id.empty()) {
            r->err = "Failed to start playback session.";
        } else {
            r->ok = true;
        }

        abs_idle([r, lgen] {
            if (lgen != g_load_gen) {
                // A newer book was selected while this one was loading
                if (!r->session_id.empty()) {
                const std::string sid = r->session_id;
                abs_async([sid] { gAbs.closeSession(sid); });
		}
                delete r;
                return;
            }
            if (!r->ok) {
                player_set_info("Load failed", "", "");
                show_abs_error(r->err);
                delete r;
                return;
            }
            ap.active = true;
            ap.offline = false;
            ap.session_id = r->session_id;
            ap.item_id = r->item_id;
            ap.book_title = r->title;
            ap.book_author = r->author;
            ap.busy_msg.clear();
            ap.book_duration = r->udur;
            abs_remember_session(r->session_id, r->item_id, r->current, r->udur);
            ap.chapters = r->chapters;
            ap.tracks = r->tracks;
            if (ap.book_duration <= 0) {
                // fall back to last track end
                ap.book_duration = ap.tracks.back().start_offset + ap.tracks.back().duration;
            }

            // Match server session tracks. Detail list is the source of truth.
            double start_at = r->current;
            int idx = 0;
            for (size_t i = 0; i < ap.tracks.size(); i++) {
                if (start_at >= ap.tracks[i].start_offset &&
                    start_at < ap.tracks[i].start_offset + ap.tracks[i].duration) {
                    idx = (int)i;
                    break;
                }
            }
            double offset = start_at - ap.tracks[idx].start_offset;
            if (offset < 0) offset = 0;

            g_print("ABS: resuming '%s' at %.0fs (track %d/%d)\n",
                    ap.book_title.c_str(), start_at, idx + 1, (int)ap.tracks.size());
            delete r;
            last_timestamp = 0;
            abs_begin_track(idx, offset);
        });
    });
}

static void abs_play_downloaded(const AbsDownload& d) {
    abs_leave_local_file();
    if (ap.active) abs_shutdown();
    backend.stop();
    ++g_load_gen;
    abs_close_ram_fds();
    ap.active = true;
    ap.offline = true;
    ap.session_id.clear();
    ap.item_id = d.item.id;
    ap.book_title = d.item.title;
    ap.book_author = d.item.author;
    ap.busy_msg.clear();
    ap.book_duration = d.item.duration;
    ap.last_sync_position = 0;
    ap.last_sync_tick = 0;
    ap.chapters = d.chapters;
    ap.tracks = d.tracks;
    player_set_book(ap.book_title, ap.book_author);
    player_set_cover(NULL);
    player_apply_display_mode();

    char key[256];
    snprintf(key, sizeof(key), "abs_download_pos_%s", ap.item_id.c_str());
    double start_at = atof(db.getSetting(key, "0").c_str());
    if (ap.book_duration > 0 && start_at >= ap.book_duration) start_at = 0;
    int idx = 0;
    for (size_t i = 0; i < ap.tracks.size(); ++i) {
        if (start_at >= ap.tracks[i].start_offset &&
            start_at < ap.tracks[i].start_offset + ap.tracks[i].duration) {
            idx = (int)i;
            break;
        }
    }
    double offset = start_at - ap.tracks[idx].start_offset;
    if (offset < 0) offset = 0;
    g_print("ABS: playing downloaded '%s' at %.0fs\n", ap.book_title.c_str(), start_at);
    abs_begin_track(idx, offset);
}

// ---------------------------------------------------------------------------------
// Time display (Book / Chapter / Track) and chapter helpers
// ---------------------------------------------------------------------------------

void abs_time_display(bool dispUpdate) {
    int idx = ap.track_idx;
    bool have_track = idx >= 0 && idx < (int)ap.tracks.size();

    double tpos = backend.get_position() / GST_SECOND;
    double book_pos = have_track ? ap.tracks[idx].start_offset + tpos : ap.book_position;
    double book_dur = ap.book_duration > 0 ? ap.book_duration : book_pos;

    const AbsChapter* ch = abs_chapter_at(book_pos);
    std::string chapter, left;
    if (!ap.busy_msg.empty()) {
        chapter = ap.busy_msg;
    } else if (ch) {
        chapter = ch->title.empty() ? "Chapter" : trunc_str(ch->title, is_hires ? 60 : 40);
        left = fmt_left_text(ch->end - book_pos, "chapter");
    } else {
        if (have_track)
            chapter = "Part " + std::to_string(idx + 1) + " of " + std::to_string(ap.tracks.size());
        left = fmt_left_text(book_dur - book_pos, "book");
    }

    if (ap.busy_msg.empty() && dispUpdate)
        player_set_info(chapter, fmt_time(book_pos) + " / " + fmt_time(book_dur), left);
    else if (ap.busy_msg.empty())
        player_set_info(chapter, "", "");
}

double abs_seconds_to_chapter_end() {
    if (!ap.active) return -1;
    double bpos = ap.book_position;
    const AbsChapter* ch = abs_chapter_at(bpos);
    if (ch) return ch->end - bpos;
    if (ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size())
        return ap.tracks[ap.track_idx].start_offset + ap.tracks[ap.track_idx].duration - bpos;
    return -1;
}

// Chapter list for the current ABS book (server chapter metadata)
void abs_show_chapter_list() {
    if (!ap.active) return;
    if (ap.chapters.empty()) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                           GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_INFO,
                                           GTK_BUTTONS_OK,
                                           "No chapter metadata for this book.");
        gtk_window_set_title(GTK_WINDOW(dialog), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    int cur_ch = -1;
    std::vector<std::string> rows, rights;
    for (size_t i = 0; i < ap.chapters.size(); i++) {
        if (ap.book_position >= ap.chapters[i].start && ap.book_position < ap.chapters[i].end)
            cur_ch = (int)i;
        rows.push_back(ap.chapters[i].title.empty() ? "Chapter " + std::to_string(i + 1)
                                                    : ap.chapters[i].title);
        rights.push_back(fmt_time(ap.chapters[i].start));
    }

    int pick = ui_pick_dialog(GTK_WINDOW(window), "Chapters", rows, rights, cur_ch);
    if (pick < 0 || pick >= (int)ap.chapters.size()) return;

    double start = ap.chapters[pick].start;
    // Find the track containing this chapter start
    for (size_t i = 0; i < ap.tracks.size(); i++) {
        if (start >= ap.tracks[i].start_offset &&
            start < ap.tracks[i].start_offset + ap.tracks[i].duration) {
            abs_begin_track((int)i, start - ap.tracks[i].start_offset);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------
// Settings (edited on the Options screen)
// ---------------------------------------------------------------------------------

struct AbsCfg {
    std::string server, user, pass, key;
};

static std::string abs_norm_server(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    if (s.empty() || s == "http://") return "";
    if (s.find("://") == std::string::npos) s = "http://" + s;
    return s;
}

static AbsCfg abs_read_cfg() {
    AbsCfg c;
    c.server = abs_norm_server(db.getSetting("abs_server", ""));
    c.user = db.getSetting("abs_user", "");
    c.pass = db.getSetting("abs_pass", "");
    c.key = db.getSetting("abs_token", "");
    return c;
}

static bool abs_cfg_complete(const AbsCfg& c) {
    return !c.server.empty() && (!c.key.empty() || (!c.user.empty() && !c.pass.empty()));
}

// Worker thread only (the client is not thread-safe)
static void abs_apply_cfg(const AbsCfg& c) {
    gAbs.setServer(c.server, c.key);
    if (c.key.empty()) gAbs.setCredentials(c.user, c.pass);
}

bool abs_settings_enabled() {
    std::string v = db.getSetting("abs_enabled", "");
    if (v.empty()) {
        // Existing installs that already have a server configured keep ABS on
        AbsCfg c = abs_read_cfg();
        return abs_cfg_complete(c);
    }
    return v == "1";
}

void abs_test_connection(AbsTestDone done, gpointer user) {
    AbsCfg c = abs_read_cfg();
    if (!abs_cfg_complete(c)) {
        done(false, "Enter the server address and a username + password (or an API key).", user);
        return;
    }
    abs_async([c, done, user] {
        abs_apply_cfg(c);
        std::string version, msg;
        bool ok = false;
        gAbs.setHttpTimeout(8);
        if (!gAbs.checkServer(version)) {
            msg = "Could not reach the server. Check Wi-Fi and the address.";
        } else if (c.key.empty() && !gAbs.login()) {
            msg = gAbs.lastError();
        } else {
            std::vector<AbsLibrary> libs;
            if (!gAbs.getLibraries(libs)) {
                msg = c.key.empty() ? "Login was rejected." : "The API key was rejected.";
            } else {
                ok = true;
                msg = "Connected: server " + version + ", " + std::to_string(libs.size()) +
                      (libs.size() == 1 ? " library" : " libraries");
            }
        }
        gAbs.setHttpTimeout(20);
        abs_idle([ok, msg, done, user] { done(ok, msg, user); });
    });
}

// ---------------------------------------------------------------------------------
// Library screen (canva mockup 2)
// ---------------------------------------------------------------------------------

static GtkWidget* lib_status = NULL;
static GtkWidget* lib_scroll = NULL;
static GtkWidget* lib_content = NULL;
static GtkWidget* lib_search_btn = NULL;
static GtkWidget* lib_view_btn = NULL;
static GtkWidget* lib_open_btn = NULL;
static GtkWidget* lib_local_btn = NULL;

static std::vector<AbsItem> abs_page_items;
static std::vector<GtkWidget*> abs_row_covers;   // cover image widget per card/row
static std::vector<Book> lib_local_books;
static std::string abs_library_id;
static std::string abs_cur_search;
static int abs_page = 0;
static int abs_total = 0;
static guint abs_page_gen = 0;      // invalidates in-flight cover callbacks
static guint abs_browser_gen = 0;   // invalidates the async connect callback
static bool abs_grid_view = true;
static bool lib_ready = false;      // a page of books is showing
static bool lib_connecting = false;
static bool lib_local_mode = false;
static bool abs_download_mode = false;
static bool abs_download_busy = false;
static std::vector<AbsDownload> abs_download_items;

// Small pages keep building fast: laying out hundreds of GTK widgets in one go
// was a major source of UI lag on the Kindle.
static int abs_page_size() { return abs_grid_view ? 6 : 10; }

static void lib_set_status(const std::string& text) {
    if (lib_status) ui_label_set(lib_status, text);
}

static void abs_invalidate_page() {
    abs_page_gen++;
    abs_browser_gen++;
    abs_row_covers.clear();
    abs_page_items.clear();
}

static void abs_clear_list() {
    if (!lib_content) return;
    GList* children = gtk_container_get_children(GTK_CONTAINER(lib_content));
    for (GList* it = children; it; it = it->next)
        gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(children);
    abs_invalidate_page();
}

// Content area helper: a fixed-height slot with the child aligned inside
static void lib_slot(GtkWidget* box, GtkWidget* child, int mock_h, double xalign = 0.5,
                     double yalign = 0.5) {
    GtkWidget* a = gtk_alignment_new((gfloat)xalign, (gfloat)yalign, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), child);
    gtk_widget_set_size_request(a, -1, ui_px(mock_h));
    gtk_box_pack_start(GTK_BOX(box), a, FALSE, FALSE, 0);
}

static void lib_show_message(const std::string& text, const char* button,
                             UiCb cb, gpointer cb_data = NULL) {
    abs_clear_list();
    GtkWidget* box = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui_vspace(160), FALSE, FALSE, 0);
    GtkWidget* l = ui_label_new(text, 32, false, false, 0.5, 820);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    if (button) {
        gtk_box_pack_start(GTK_BOX(box), ui_vspace(40), FALSE, FALSE, 0);
        GtkWidget* b = ui_button_new(button, 32, 380, 84, 4, 12);
        ui_connect(b, cb, cb_data);
        lib_slot(box, b, 84);
    }
    GtkWidget* a = gtk_alignment_new(0.5, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), box);
    gtk_box_pack_start(GTK_BOX(lib_content), a, TRUE, TRUE, 0);
    gtk_widget_show_all(a);
    lib_set_status("");
    ui_scroll_to_top(lib_scroll);
    app_schedule_refresh(false);
}

static void lib_update_toolbar() {
    if (lib_local_mode) {
        gtk_widget_hide(lib_search_btn);
        gtk_widget_hide(lib_view_btn);
        gtk_widget_show(lib_local_btn);
        ui_button_set_text(lib_local_btn, "ABS");
        gtk_widget_show(lib_open_btn);
        ui_button_set_text(lib_open_btn, "Open file");
    } else {
        gtk_widget_show(lib_search_btn);
        gtk_widget_show(lib_view_btn);
        gtk_widget_show(lib_local_btn);
        ui_button_set_text(lib_local_btn, "Local");
        gtk_widget_show(lib_open_btn);
        ui_button_set_text(lib_open_btn, abs_download_mode ? "Library" : "Downloads");
        ui_button_set_text(lib_search_btn, abs_cur_search.empty() ? "Search" : "Clear");
        ui_button_set_text(lib_view_btn, abs_grid_view ? "List" : "Grid");
    }
}

static void abs_load_page(int page, const std::string& search, bool keep_visible = false);
static void lib_connect_and_load(const AbsCfg& c);
static void lib_show_downloads();
static void lib_show_local();

// --- cards -------------------------------------------------------------------------

// Cover size (mockup px) for the active view
static void lib_cover_box(int* w, int* h) {
    *w = abs_grid_view ? 316 : 150;
    *h = abs_grid_view ? 340 : 150;
}

static GtkWidget* lib_cover_new(int box_w, int box_h) {
    GtkWidget* img = gtk_image_new();
    GdkPixbuf* ph = gdk_pixbuf_new_from_inline(-1, audiobook, FALSE, NULL);
    if (ph) {
        int pw = gdk_pixbuf_get_width(ph), phh = gdk_pixbuf_get_height(ph);
        double sc = MIN((double)ui_px(box_w) * 0.6 / pw, (double)ui_px(box_h) * 0.6 / phh);
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(ph, MAX(1, (int)(pw * sc)),
                                                    MAX(1, (int)(phh * sc)), GDK_INTERP_BILINEAR);
        gtk_image_set_from_pixbuf(GTK_IMAGE(img), scaled);
        g_object_unref(scaled);
        g_object_unref(ph);
    }
    GtkWidget* a = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), img);
    gtk_widget_set_size_request(a, ui_px(box_w), ui_px(box_h));
    g_object_set_data(G_OBJECT(a), "img", img);
    return a;
}

static void on_lib_play(GtkWidget*, gpointer idx) {
    if (abs_download_mode) {
        size_t i = (size_t)GPOINTER_TO_INT(idx);
        if (i >= abs_download_items.size()) return;
        abs_play_downloaded(abs_download_items[i]);
        app_show_tab(TAB_PLAY);
        return;
    }
    size_t i = (size_t)GPOINTER_TO_INT(idx);
    if (i >= abs_page_items.size()) return;
    AbsItem it = abs_page_items[i];
    abs_play_item(it.id, it.title, it.author);
    app_show_tab(TAB_PLAY);
}

static void on_lib_download(GtkWidget*, gpointer idx) {
    size_t i = (size_t)GPOINTER_TO_INT(idx);
    if (i >= abs_page_items.size()) return;
    if (abs_download_busy) {
        lib_set_status("A download is already running.");
        return;
    }
    AbsItem item = abs_page_items[i];
    AbsCfg cfg = abs_read_cfg();
    abs_download_busy = true;
    lib_set_status("Preparing download...");
    abs_async([item, cfg] {
        abs_apply_cfg(cfg);
        AbsDownload d;
        d.item = item;
        double current = 0, user_duration = 0;
        std::vector<AbsTrack> source_tracks;
        bool ok = gAbs.getItemDetail(item.id, d.chapters, source_tracks, current, user_duration) &&
                  !source_tracks.empty();
        if (ok && user_duration > 0) d.item.duration = user_duration;
        std::string session_id;
        if (ok) ok = gAbs.startPlayback(item.id, session_id, d.tracks) &&
                     !session_id.empty() && !d.tracks.empty();
        if (ok) mkdir(ABS_CACHE_DIR, 0777);
        std::vector<std::string> new_track_paths;
        for (size_t n = 0; ok && n < d.tracks.size(); ++n) {
            const size_t part_number = n + 1;
            const size_t part_total = d.tracks.size();
            const std::string download_title = item.title;
            abs_idle([part_number, part_total, download_title] {
                char msg[160];
                snprintf(msg, sizeof(msg), "Downloading %s (%zu/%zu parts)...",
                         download_title.c_str(), part_number, part_total);
                lib_set_status(msg);
            });
            std::string path = abs_download_track_path(item.id, d.tracks[n].index);
            if (path.empty()) { ok = false; break; }
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
                std::string part = path + ".part";
                remove(part.c_str());
                long long max_bytes = 0;
                if (!abs_download_space_budget(max_bytes)) { ok = false; break; }
                int fd = open(part.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
                long long bytes = 0;
                ok = fd >= 0 && gAbs.downloadToFd(d.tracks[n].content_url, fd, max_bytes, bytes);
                if (fd >= 0) close(fd);
                ok = ok && bytes > 0 && rename(part.c_str(), path.c_str()) == 0;
                if (ok) new_track_paths.push_back(path);
                else remove(part.c_str());
            }
        }
        if (!session_id.empty() && !gAbs.closeSession(session_id))
            g_print("ABS: could not close download session %s\n", session_id.c_str());
        if (ok) ok = abs_save_download_manifest(d);
        if (!ok) {
            // Keep a failed or space-limited attempt from leaving unlisted
            // audio files behind. Pre-existing playback caches are untouched.
            for (size_t i = 0; i < new_track_paths.size(); ++i)
                remove(new_track_paths[i].c_str());
        }
        const std::string title = item.title;
        abs_idle([ok, title] {
            abs_download_busy = false;
            if (ok) {
                lib_set_status(("Downloaded " + title + " for offline listening.").c_str());
                if (abs_download_mode) lib_show_downloads();
            } else {
                lib_set_status(("Download failed for " + title + ". Check Wi-Fi and free space.").c_str());
            }
        });
    });
}

static std::string lib_listened_text(const AbsItem& it) {
    if (it.finished) return "Finished";
    if (it.progress < 0) return " ";
    if (it.duration > 0 && it.progress > 0) {
        int pct = (int)(it.progress * 100 / it.duration);
        if (pct < 1) pct = 1;
        if (pct > 99) pct = 99;
        return std::to_string(pct) + "% listened";
    }
    return "Not started";
}

static GtkWidget* lib_make_cell(const AbsItem& it, size_t index) {
    GtkWidget* vb = gtk_vbox_new(FALSE, 0);

    int cw, ch;
    lib_cover_box(&cw, &ch);
    GtkWidget* cover = lib_cover_new(cw, ch);
    abs_row_covers[index] = GTK_WIDGET(g_object_get_data(G_OBJECT(cover), "img"));
    gtk_box_pack_start(GTK_BOX(vb), cover, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), ui_vspace(8), FALSE, FALSE, 0);

    lib_slot(vb, ui_label_new(trunc_str(it.title, 90), 28, false, false, 0.5, 440, 26, 3), 106, 0.5, 0.0);
    lib_slot(vb, ui_label_new(it.duration > 0 ? fmt_time(it.duration) : " ", 28), 34);
    lib_slot(vb, ui_label_new(it.num_chapters > 0 ? std::to_string(it.num_chapters) +
                              (it.num_chapters == 1 ? " chapter" : " chapters") : " ", 28), 34);
    lib_slot(vb, ui_label_new(lib_listened_text(it), 28), 34);
    gtk_box_pack_start(GTK_BOX(vb), ui_vspace(12), FALSE, FALSE, 0);

    GtkWidget* play = ui_button_new("PLAY", 40, 320, 80, 5, 14);
    ui_button_set_icon(play, UI_ICON_PLAY, 62);
    ui_connect(play, on_lib_play, GINT_TO_POINTER((int)index));
    lib_slot(vb, play, 80);
    GtkWidget* download = ui_button_new("DOWNLOAD", 28, 320, 66, 4, 10);
    ui_connect(download, on_lib_download, GINT_TO_POINTER((int)index));
    lib_slot(vb, download, 66);
    gtk_box_pack_start(GTK_BOX(vb), ui_vspace(24), FALSE, FALSE, 0);

    GtkWidget* a = gtk_alignment_new(0.5, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), vb);
    return a;
}

static GtkWidget* lib_make_row(const AbsItem& it, size_t index) {
    GtkWidget* row = gtk_hbox_new(FALSE, ui_px(24));

    int cw, ch;
    lib_cover_box(&cw, &ch);
    GtkWidget* cover = lib_cover_new(cw, ch);
    abs_row_covers[index] = GTK_WIDGET(g_object_get_data(G_OBJECT(cover), "img"));
    gtk_box_pack_start(GTK_BOX(row), cover, FALSE, FALSE, 0);

    GtkWidget* info = gtk_vbox_new(FALSE, ui_px(4));
    gtk_box_pack_start(GTK_BOX(info), ui_label_new(trunc_str(it.title, 90), 30, true, false, 0.0, 500, 26, 2),
                       FALSE, FALSE, 0);
    if (!it.author.empty())
        gtk_box_pack_start(GTK_BOX(info), ui_label_new(trunc_str(it.author, 50), 26, false, true, 0.0, 500, 51),
                           FALSE, FALSE, 0);
    std::string prog = abs_progress_text(it);
    if (!prog.empty())
        gtk_box_pack_start(GTK_BOX(info), ui_label_new(prog, 24, false, false, 0.0, 500, 77),
                           FALSE, FALSE, 0);
    GtkWidget* info_a = gtk_alignment_new(0, 0.5, 1, 0);
    gtk_widget_set_size_request(info_a, ui_px(360), -1);
    gtk_container_add(GTK_CONTAINER(info_a), info);
    gtk_box_pack_start(GTK_BOX(row), info_a, TRUE, TRUE, 0);

    GtkWidget* actions = gtk_vbox_new(FALSE, ui_px(8));
    GtkWidget* play = ui_button_new("PLAY", 30, 165, 64, 4, 10);
    ui_button_set_icon(play, UI_ICON_PLAY, 44);
    ui_connect(play, on_lib_play, GINT_TO_POINTER((int)index));
    gtk_box_pack_start(GTK_BOX(actions), play, FALSE, FALSE, 0);
    GtkWidget* download = ui_button_new("SAVE", 26, 165, 58, 3, 8);
    ui_connect(download, on_lib_download, GINT_TO_POINTER((int)index));
    gtk_box_pack_start(GTK_BOX(actions), download, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), actions, FALSE, FALSE, 0);

    GtkWidget* pad = gtk_alignment_new(0, 0, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), ui_px(14), ui_px(14), ui_px(30), ui_px(30));
    gtk_container_add(GTK_CONTAINER(pad), row);
    return pad;
}

static void on_lib_remove_download(GtkWidget*, gpointer idx) {
    size_t i = (size_t)GPOINTER_TO_INT(idx);
    if (i >= abs_download_items.size()) return;
    const AbsDownload d = abs_download_items[i];
    if (ap.active && ap.item_id == d.item.id) {
        lib_set_status("Stop this book before removing its download.");
        return;
    }
    GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Remove the downloaded copy of %s?", d.item.title.c_str());
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == GTK_RESPONSE_YES) {
        abs_remove_download(d);
        lib_show_downloads();
    }
}

static void lib_show_downloads() {
    abs_download_mode = true;
    lib_update_toolbar();
    abs_clear_list();
    abs_download_items = abs_list_downloads();
    if (abs_download_items.empty()) {
        lib_show_message("No books downloaded yet.\nUse DOWNLOAD beside a library book to save it for offline listening.",
                         NULL, NULL);
        lib_set_status("Downloads");
        return;
    }
    for (size_t i = 0; i < abs_download_items.size(); ++i) {
        const AbsDownload& d = abs_download_items[i];
        GtkWidget* row = gtk_hbox_new(FALSE, ui_px(24));
        GtkWidget* info = gtk_vbox_new(FALSE, ui_px(4));
        gtk_box_pack_start(GTK_BOX(info),
            ui_label_new(trunc_str(d.item.title, 90), 30, true, false, 0.0, 520, 26, 2),
            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(info),
            ui_label_new(d.item.author, 25, false, true, 0.0, 520, 51), FALSE, FALSE, 0);
        char detail[96];
        snprintf(detail, sizeof(detail), "Offline · %zu parts · %s", d.tracks.size(), fmt_time(d.item.duration).c_str());
        gtk_box_pack_start(GTK_BOX(info), ui_label_new(detail, 24, false, false, 0.0, 520, 77), FALSE, FALSE, 0);
        GtkWidget* info_a = gtk_alignment_new(0, 0.5, 1, 0);
        gtk_container_add(GTK_CONTAINER(info_a), info);
        gtk_box_pack_start(GTK_BOX(row), info_a, TRUE, TRUE, 0);

        GtkWidget* actions = gtk_vbox_new(FALSE, ui_px(8));
        GtkWidget* play = ui_button_new("PLAY", 30, 165, 64, 4, 10);
        ui_button_set_icon(play, UI_ICON_PLAY, 42);
        ui_connect(play, on_lib_play, GINT_TO_POINTER((int)i));
        gtk_box_pack_start(GTK_BOX(actions), play, FALSE, FALSE, 0);
        GtkWidget* remove_btn = ui_button_new("REMOVE", 24, 165, 56, 3, 8);
        ui_connect(remove_btn, on_lib_remove_download, GINT_TO_POINTER((int)i));
        gtk_box_pack_start(GTK_BOX(actions), remove_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), actions, FALSE, FALSE, 0);

        GtkWidget* pad = gtk_alignment_new(0, 0.5, 1, 1);
        gtk_alignment_set_padding(GTK_ALIGNMENT(pad), ui_px(14), ui_px(14), ui_px(30), ui_px(30));
        gtk_container_add(GTK_CONTAINER(pad), row);
        gtk_box_pack_start(GTK_BOX(lib_content), pad, FALSE, FALSE, 0);
        if (i + 1 < abs_download_items.size())
            gtk_box_pack_start(GTK_BOX(lib_content), ui_rule(2), FALSE, FALSE, 0);
    }
    gtk_widget_show_all(lib_content);
    lib_set_status((std::to_string(abs_download_items.size()) + " downloaded books").c_str());
    ui_scroll_to_top(lib_scroll);
    app_schedule_refresh(false);
}

static void on_lib_downloads(GtkWidget*, gpointer) {
    if (abs_download_mode) {
        abs_download_mode = false;
        lib_update_toolbar();
        if (lib_ready) abs_load_page(abs_page, abs_cur_search);
        else lib_page_activate();
    } else {
        lib_show_downloads();
    }
}

static void on_lib_prev(GtkWidget*, gpointer) {
    if (abs_page > 0) abs_load_page(abs_page - 1, abs_cur_search);
}

static void on_lib_next(GtkWidget*, gpointer) {
    abs_load_page(abs_page + 1, abs_cur_search);
}

static void lib_add_pager(int pages) {
    if (pages <= 1) return;
    GtkWidget* row = gtk_hbox_new(FALSE, ui_px(24));
    GtkWidget* prev = ui_button_new("< Prev", 30, 230, 72, 3, 10);
    ui_connect(prev, on_lib_prev, NULL);
    GtkWidget* next = ui_button_new("Next >", 30, 230, 72, 3, 10);
    ui_connect(next, on_lib_next, NULL);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d / %d", abs_page + 1, pages);
    GtkWidget* cnt = ui_label_new(buf, 28);
    gtk_widget_set_size_request(cnt, ui_px(160), -1);
    if (abs_page > 0) gtk_box_pack_start(GTK_BOX(row), prev, FALSE, FALSE, 0);
    else gtk_box_pack_start(GTK_BOX(row), ui_hspace(230), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), cnt, FALSE, FALSE, 0);
    if (abs_page + 1 < pages) gtk_box_pack_start(GTK_BOX(row), next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ui_hspace(1), FALSE, FALSE, 0);
    GtkWidget* a = gtk_alignment_new(0.5, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), row);
    gtk_box_pack_start(GTK_BOX(lib_content), ui_vspace(10), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lib_content), a, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lib_content), ui_vspace(40), FALSE, FALSE, 0);
}

static void lib_fetch_covers(const std::vector<AbsItem>& items, guint gen) {
    int cw, ch;
    lib_cover_box(&cw, &ch);
    int box_w = ui_px(cw), box_h = ui_px(ch);
    abs_async([gen, items, box_w, box_h] {
        for (size_t i = 0; i < items.size(); i++) {
            std::string cover = std::string(ABS_COVER_DIR) + "/" + items[i].id + ".jpg";
            if (!abs_is_jpeg_file(cover)) {
                remove(cover.c_str());
                mkdir(ABS_COVER_DIR, 0777);
                if (!gAbs.getCover(items[i].id, 400, cover)) {
                    remove(cover.c_str());   // don't cache failed fetches
                    continue;
                }
                // Sanity check: must be a real JPEG (server may send other
                // formats; a corrupt/garbage file would fail pixbuf decode)
                if (!abs_is_jpeg_file(cover)) {
                    g_print("ABS: cover %s not a JPEG, dropping\n", cover.c_str());
                    remove(cover.c_str());
                    continue;
                }
            }
            std::string path = cover;
            size_t idx = i;
            abs_idle([gen, idx, path, box_w, box_h] {
                if (gen != abs_page_gen) return;   // page changed meanwhile
                if (idx >= abs_row_covers.size() || !abs_row_covers[idx]) return;
                GdkPixbuf* pix = gdk_pixbuf_new_from_file(path.c_str(), NULL);
                if (!pix) return;
                int pw = gdk_pixbuf_get_width(pix), ph = gdk_pixbuf_get_height(pix);
                double sc = (pw > 0 && ph > 0) ? MIN((double)box_w / pw, (double)box_h / ph) : 1.0;
                GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
                    pix, MAX(1, (int)(pw * sc)), MAX(1, (int)(ph * sc)), GDK_INTERP_BILINEAR);
                gtk_image_set_from_pixbuf(GTK_IMAGE(abs_row_covers[idx]), scaled);
                g_object_unref(scaled);
                g_object_unref(pix);
            });
        }
    });
}

static void lib_fill_page(std::vector<AbsItem> items, int total, int page,
                          const std::string& search, guint gen) {
    abs_total = total > 0 ? total : (int)items.size();
    abs_page_items = items;
    abs_cur_search = search;
    abs_row_covers.assign(items.size(), NULL);
    abs_page = page;
    lib_update_toolbar();

    if (items.empty()) {
        lib_show_message(search.empty() ? "This library is empty." : "No books match your search.",
                         NULL, NULL);
        return;
    }

    int pages = (abs_total + abs_page_size() - 1) / abs_page_size();
    char count_buf[96];
    if (search.empty())
        snprintf(count_buf, sizeof(count_buf), "%d books%s", abs_total,
                 pages > 1 ? "  (scrollable for more)" : "");
    else
        snprintf(count_buf, sizeof(count_buf), "%d result%s", abs_total, abs_total == 1 ? "" : "s");
    lib_set_status(count_buf);

    if (abs_grid_view) {
        GtkWidget* grid = gtk_table_new((items.size() + 1) / 2, 2, TRUE);
        for (size_t i = 0; i < items.size(); i++)
            gtk_table_attach(GTK_TABLE(grid), lib_make_cell(items[i], i),
                             i % 2, i % 2 + 1, i / 2, i / 2 + 1,
                             (GtkAttachOptions)(GTK_EXPAND | GTK_FILL), GTK_FILL, 0, 0);
        gtk_box_pack_start(GTK_BOX(lib_content), grid, FALSE, FALSE, 0);
    } else {
        for (size_t i = 0; i < items.size(); i++) {
            gtk_box_pack_start(GTK_BOX(lib_content), lib_make_row(items[i], i), FALSE, FALSE, 0);
            if (i + 1 < items.size())
                gtk_box_pack_start(GTK_BOX(lib_content), ui_rule(2), FALSE, FALSE, 0);
        }
    }
    lib_add_pager(pages);
    gtk_widget_show_all(lib_content);
    ui_scroll_to_top(lib_scroll);
    app_schedule_refresh(false);

    lib_fetch_covers(items, gen);
}

static void on_lib_retry(GtkWidget*, gpointer) {
    lib_ready = false;
    lib_page_activate();
}

static void abs_load_page(int page, const std::string& search, bool keep_visible) {
    if (keep_visible && !abs_page_items.empty()) {
        ++abs_page_gen;
        lib_set_status("Refreshing library...");
    } else {
        abs_clear_list();
        lib_show_message("Loading...", NULL, NULL);
    }
    guint gen = abs_page_gen;

    std::string lib = abs_library_id;
    int limit = abs_page_size();
    abs_async([gen, page, search, lib, limit, keep_visible] {
        std::vector<AbsItem> items;
        int total = -1;
        bool ok;
        if (!search.empty()) {
            // Server-side search returns everything at once; page the
            // results client-side so a big match set doesn't stall the UI.
            ok = gAbs.getItems(lib, 0, limit, search, items, total);
            if (ok) {
                total = (int)items.size();
                int start = page * limit;
                std::vector<AbsItem> slice;
                if (start < total)
                    slice.assign(items.begin() + start,
                                 items.begin() + MIN(total, start + limit));
                items.swap(slice);
            }
        } else {
            ok = gAbs.getItems(lib, page, limit, search, items, total);
        }
        abs_idle([gen, page, search, ok, items, total, keep_visible] {
            if (gen != abs_page_gen) return;
            if (!ok) {
                if (keep_visible && !abs_page_items.empty()) {
                    lib_ready = true;
                    lib_set_status("Refresh failed; showing previous results.");
                } else {
                    lib_ready = false;
                    lib_show_message("Failed to reach the server.\nCheck Wi-Fi, the server address and your login.",
                                     "Retry", on_lib_retry);
                }
                return;
            }
            // lib_fill_page clears the "Loading..." message itself
            abs_clear_list();
            guint g2 = abs_page_gen;
            lib_fill_page(items, total, page, search, g2);
        });
    });
}

static void lib_connect_and_load(const AbsCfg& c) {
    g_print("ABS: connecting to %s\n", c.server.c_str());
    lib_connecting = true;
    lib_show_message("Connecting to server...", NULL, NULL);
    guint bgen = ++abs_browser_gen;
    std::string saved_lib = db.getSetting("abs_library_id", "");
    abs_library_id = saved_lib;
    abs_async([c, saved_lib, bgen] {
        abs_apply_cfg(c);
        std::string version, err;
        bool ok = false;
        // The first request after app start occasionally times out on the
        // Kindle's Wi-Fi (power save), so retry a few times before giving up.
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            gAbs.setHttpTimeout(attempt == 0 ? 8 : 15);
            ok = gAbs.checkServer(version);
            if (!ok && attempt < 2) sleep(2);
        }
        gAbs.setHttpTimeout(20);
        if (!ok) err = "Server unreachable.\nCheck Wi-Fi and the server address.";
        if (ok && c.key.empty() && !gAbs.login()) { ok = false; err = gAbs.lastError(); }
        std::string lib = saved_lib;
        if (ok && lib.empty()) {
            std::vector<AbsLibrary> libs;
            if (gAbs.getLibraries(libs) && !libs.empty()) lib = libs[0].id;
            else { ok = false; err = "No libraries found on the server."; }
        }
        abs_idle([bgen, ok, lib, err] {
            if (bgen != abs_browser_gen) return;   // settings changed meanwhile
            lib_connecting = false;
            if (!ok) {
                lib_show_message(err, "Retry", on_lib_retry);
                return;
            }
            if (lib != db.getSetting("abs_library_id", ""))
                db.setSetting("abs_library_id", lib);
            abs_library_id = lib;
            lib_ready = true;
            abs_load_page(0, "");
        });
    });
}

// --- local books (Audiobookshelf switched off) -----------------------------------

static void on_local_play(GtkWidget*, gpointer idx) {
    size_t i = (size_t)GPOINTER_TO_INT(idx);
    if (i >= lib_local_books.size()) return;
    std::string path = lib_local_books[i].filepath;
    app_open_local_file(path.c_str());
}

static void on_lib_action(GtkWidget*, gpointer) {
    if (lib_local_mode) app_open_file_dialog();
    else on_lib_downloads(NULL, NULL);
}

static void on_lib_local(GtkWidget*, gpointer) {
    if (lib_local_mode) {
        lib_local_mode = false;
        abs_download_mode = false;
        lib_update_toolbar();
        if (lib_ready) abs_load_page(abs_page, abs_cur_search);
        else lib_page_activate();
    } else {
        abs_download_mode = false;
        lib_show_local();
    }
}

static void lib_show_local() {
    lib_local_mode = true;
    lib_update_toolbar();
    lib_local_books = db.getHistory();

    if (lib_local_books.empty()) {
        lib_show_message("No books yet.\nTap \"Open file\" to pick an M4B or MP3 audiobook.",
                         NULL, NULL);
        lib_set_status("Local books");
        return;
    }
    abs_clear_list();
    for (size_t i = 0; i < lib_local_books.size(); i++) {
        const Book& b = lib_local_books[i];
        GtkWidget* row = gtk_hbox_new(FALSE, ui_px(24));
        GtkWidget* info = gtk_vbox_new(FALSE, ui_px(6));
        gtk_box_pack_start(GTK_BOX(info),
                           ui_label_new(trunc_str(get_display_name(b.filepath), 90), 30, true, false, 0.0, 620, 26, 2),
                           FALSE, FALSE, 0);
        std::string prog = b.duration > 0 ? fmt_time(b.last_position) + " / " + fmt_time(b.duration)
                                          : fmt_time(b.last_position);
        gtk_box_pack_start(GTK_BOX(info), ui_label_new(prog, 26, false, false, 0.0, 620, 77), FALSE, FALSE, 0);
        GtkWidget* info_a = gtk_alignment_new(0, 0.5, 1, 0);
        gtk_container_add(GTK_CONTAINER(info_a), info);
        gtk_box_pack_start(GTK_BOX(row), info_a, TRUE, TRUE, 0);

        GtkWidget* play = ui_button_new("PLAY", 32, 210, 72, 4, 12);
        ui_button_set_icon(play, UI_ICON_PLAY, 44);
        ui_connect(play, on_local_play, GINT_TO_POINTER((int)i));
        GtkWidget* play_a = gtk_alignment_new(0.5, 0.5, 0, 0);
        gtk_container_add(GTK_CONTAINER(play_a), play);
        gtk_box_pack_start(GTK_BOX(row), play_a, FALSE, FALSE, 0);

        GtkWidget* pad = gtk_alignment_new(0, 0, 1, 1);
        gtk_alignment_set_padding(GTK_ALIGNMENT(pad), ui_px(22), ui_px(22), ui_px(30), ui_px(30));
        gtk_container_add(GTK_CONTAINER(pad), row);
        gtk_box_pack_start(GTK_BOX(lib_content), pad, FALSE, FALSE, 0);
        if (i + 1 < lib_local_books.size())
            gtk_box_pack_start(GTK_BOX(lib_content), ui_rule(2), FALSE, FALSE, 0);
    }
    gtk_widget_show_all(lib_content);
    lib_set_status("Local books");
    ui_scroll_to_top(lib_scroll);
}

// --- toolbar -----------------------------------------------------------------------

static void on_lib_search(GtkWidget*, gpointer) {
    if (!abs_cur_search.empty()) {   // "Clear"
        abs_load_page(0, "");
        return;
    }
    app_keyboard("Search library", "", false, [](bool ok, const std::string& q) {
        if (ok && !q.empty()) abs_load_page(0, q);
    });
}

static void on_lib_view(GtkWidget*, gpointer) {
    abs_grid_view = !abs_grid_view;
    db.setSetting("abs_view", abs_grid_view ? "grid" : "list");
    abs_load_page(0, abs_cur_search);
}

static void on_lib_goto_options(GtkWidget*, gpointer) { app_show_tab(TAB_OPTIONS); }

GtkWidget* lib_page_new() {
    abs_grid_view = db.getSetting("abs_view", "grid") != "list";

    GtkWidget* page = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui_inset_new(), FALSE, FALSE, 0);

    // Toolbar: count at the left, Search / view switch at the right
    GtkWidget* bar = gtk_hbox_new(FALSE, ui_px(16));
    gtk_box_pack_start(GTK_BOX(bar), ui_hspace(14), FALSE, FALSE, 0);
    lib_status = ui_label_new("", 26, false, false, 0.0, 0, 77);
    gtk_box_pack_start(GTK_BOX(bar), lib_status, TRUE, TRUE, 0);
    lib_search_btn = ui_button_new("Search", 26, 150, 60, 3, 8);
    ui_connect(lib_search_btn, on_lib_search, NULL);
    lib_view_btn = ui_button_new(abs_grid_view ? "List" : "Grid", 26, 130, 60, 3, 8);
    ui_connect(lib_view_btn, on_lib_view, NULL);
    lib_local_btn = ui_button_new("Local", 26, 100, 60, 3, 8);
    ui_connect(lib_local_btn, on_lib_local, NULL);
    lib_open_btn = ui_button_new("Open file", 26, 135, 60, 3, 8);
    ui_connect(lib_open_btn, on_lib_action, NULL);
    GtkWidget* btns[4] = { lib_search_btn, lib_view_btn, lib_local_btn, lib_open_btn };
    for (int i = 0; i < 4; i++) {
        GtkWidget* a = gtk_alignment_new(0.5, 0.5, 0, 0);
        gtk_container_add(GTK_CONTAINER(a), btns[i]);
        gtk_box_pack_start(GTK_BOX(bar), a, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(bar), ui_hspace(14), FALSE, FALSE, 0);
    GtkWidget* bar_a = gtk_alignment_new(0, 0.5, 1, 0);
    gtk_container_add(GTK_CONTAINER(bar_a), bar);
    gtk_widget_set_size_request(bar_a, -1, ui_px(76));
    gtk_box_pack_start(GTK_BOX(page), bar_a, FALSE, FALSE, 0);

    lib_content = gtk_vbox_new(FALSE, 0);
    lib_scroll = ui_scroll_new(lib_content);
    gtk_box_pack_start(GTK_BOX(page), lib_scroll, TRUE, TRUE, 0);
    return page;
}

void lib_page_activate() {
    // Local downloads remain available even when the network ABS library has
    // been disabled or is not configured.
    if (abs_download_mode) {
        lib_local_mode = false;
        lib_show_downloads();
        return;
    }
    if (!abs_settings_enabled()) {
        lib_show_local();
        return;
    }
    if (lib_local_mode) {      // coming back from the local view
        lib_local_mode = false;
        lib_ready = false;
    }
    lib_update_toolbar();
    AbsCfg c = abs_read_cfg();
    if (!abs_cfg_complete(c)) {
        lib_ready = false;
        lib_show_message("Audiobookshelf isn't set up yet.\nEnter your server address and login in Options.",
                         "Go to Options", on_lib_goto_options);
        return;
    }
    if (lib_connecting) return;   // a load is already in flight
    if (!lib_ready) {
        abs_cur_search.clear();
        lib_connect_and_load(c);
        return;
    }
    // Already connected: refresh the current page every time this tab is
    // (re)shown, instead of leaving a stale snapshot up - progress changed by
    // listening elsewhere in the app, or by another Audiobookshelf client,
    // should show up as soon as you come back to the Library.
    abs_load_page(abs_page, abs_cur_search, !abs_page_items.empty());
}

void abs_settings_changed() {
    // The library reconnects (new server/login) the next time it is shown
    lib_ready = false;
    lib_connecting = false;
    abs_cur_search.clear();
    db.setSetting("abs_library_id", "");
    abs_library_id.clear();
    abs_invalidate_page();
}

// ---------------------------------------------------------------------------------
// Debug/testing autostart (env-var gated; used for remote SSH testing)
// ---------------------------------------------------------------------------------

static gboolean abs_debug_autoplay_timer(gpointer data) {
    (void)data;
    static int tries = 0;
    if (lib_ready && !abs_page_items.empty()) {
        int row = 0;
        const char* r = getenv("LARK_ABS_ROW");
        if (r) {
            row = atoi(r);
            if (row < 0 || row >= (int)abs_page_items.size()) row = 0;
        }
        g_print("ABS: debug autoplay of item %d\n", row);
        AbsItem it = abs_page_items[row];
        abs_play_item(it.id, it.title, it.author);
        app_show_tab(TAB_PLAY);
        return FALSE;   // done
    }
    return (++tries < 30);   // page not loaded yet: retry every second
}

void abs_debug_autostart() {
    if (!getenv("LARK_ABS_TEST")) return;
    g_print("ABS: debug autostart - opening library\n");
    app_show_tab(TAB_LIBRARY);
    if (getenv("LARK_ABS_AUTOPLAY"))
        g_timeout_add_seconds(4, abs_debug_autoplay_timer, NULL);
}
