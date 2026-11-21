#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <string>
#include <fstream>

#include <map>

#include <execinfo.h>
#include <signal.h>
#include <ctime>
#include <cctype>
#include <algorithm>

#include "music_backend.h"
#include "database_manager.h"
#include "abs_ui.h"
#include "app_actions.h"
#include "options_ui.h"
#include "keyboard_ui.h"
#include "ui_kit.h"
#include "eink_refresh.h"
#include "openlipc/openlipc.h"

#include "icons.h"

#define DESKTOP_W_SIZE 600
#define DESKTOP_H_SIZE 800

MusicBackend backend;
DatabaseManager db;
GtkWidget *window;

// Player screen widgets (also used by abs_ui.cpp through the player_* API)
GtkWidget *cover_image;
GtkWidget *title_label;
GtkWidget *artist_label;
GtkWidget *chapter_label;
GtkWidget *time_label;
GtkWidget *left_label;

static GtkWidget *g_notebook = NULL;
static GtkWidget *g_tabbar = NULL;
static GtkWidget *g_play_glyph = NULL;
static GtkWidget *g_title_block = NULL;
static GtkWidget *g_sleep_badge = NULL;

bool user_is_seeking = false;
std::string current_file;
int last_timestamp = 0;
int flIntensity = 0;
bool dispUpdate = true;
bool is_hires = true;

// Cover box on the Player screen (mockup px)
static const int COVER_BOX_W = 440;
static const int COVER_BOX_H = 440;

static LIPC * lipcInstance = 0;
static GDBusConnection* avrcp_bus = NULL;
static gint64 avrcp_last_command_us = 0;

// Debug helper: print a widget's allocation (LARK_DUMP_ALLOC=1) for tap testing
static void dump_alloc(const char* name, GtkWidget* w) {
    if (!getenv("LARK_DUMP_ALLOC") || !w) return;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    gint wx = 0, wy = 0;
    GtkWidget* tl = gtk_widget_get_toplevel(w);
    if (tl && gtk_widget_is_toplevel(tl))
        gtk_window_get_position(GTK_WINDOW(tl), &wx, &wy);
    g_print("ALLOC %s: win(%d,%d) %dx%d\n", name, wx + a.x, wy + a.y, a.width, a.height);
}

// Crash handler: print a backtrace so remote debugging is possible
static void crash_handler(int sig) {
    void* bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "\nLARK CRASH: signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(bt, n, 2);
    _exit(sig);
}

void openLipcInstance() {
    if (lipcInstance == 0) {
        lipcInstance = LipcOpen("com.kbarni.larkplayer");
    }
}

void closeLipcInstance() {
    if (lipcInstance != 0) {
        LipcClose(lipcInstance);
    }
}

void enableSleep() {
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","preventScreenSaver",0);
}

void disableSleep() {
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","preventScreenSaver",1);
}

// ---------------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------------

static std::string fmt_clock(double secs) {
    if (secs < 0) secs = 0;
    int s = (int)secs;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
    return buf;
}

std::string fmt_left_text(double secs, const char* what) {
    if (secs < 0) secs = 0;
    int m = (int)((secs + 59) / 60);
    char buf[96];
    if (secs < 60)
        snprintf(buf, sizeof(buf), "less than a minute left in %s", what);
    else if (m == 1)
        snprintf(buf, sizeof(buf), "1 minute left in %s", what);
    else if (m < 60)
        snprintf(buf, sizeof(buf), "%d minutes left in %s", m, what);
    else
        snprintf(buf, sizeof(buf), "%dh %02dm left in %s", m / 60, m % 60, what);
    return buf;
}

// ---------------------------------------------------------------------------------
// Settings-backed switches and the sleep timer
// ---------------------------------------------------------------------------------

static const SleepOption kSleepOptions[] = {
    { "Off", 0 }, { "End of Chapter", -1 },
    { "5m", 5 }, { "10m", 10 }, { "15m", 15 }, { "20m", 20 },
    { "25m", 25 }, { "30m", 30 }, { "45m", 45 }, { "1hr", 60 }
};
static const int kSleepOptionCount = (int)(sizeof(kSleepOptions) / sizeof(kSleepOptions[0]));

static int    g_sleep_mode = 0;       // index into kSleepOptions
static time_t g_sleep_end  = 0;
static time_t g_suspend_at = 0;       // auto-suspend countdown after the timer fired
static bool   g_suspend_released = false;   // screensaver lock released for suspend
static time_t g_suspend_requested_at = 0;   // time actual suspend was requested

static const int AUTO_SUSPEND_SECS = 10 * 60;

int app_sleep_option_count() { return kSleepOptionCount; }
const SleepOption& app_sleep_option(int index) {
    if (index < 0 || index >= kSleepOptionCount) index = 0;
    return kSleepOptions[index];
}
int app_sleep_mode() { return g_sleep_mode; }

static void update_sleep_badge() {
    if (!g_sleep_badge) return;
    std::string txt;
    if (g_sleep_mode > 0) {
        const SleepOption& o = kSleepOptions[g_sleep_mode];
        if (o.minutes < 0) {
            txt = "Sleep timer: end of chapter";
        } else {
            int rem = (int)(g_sleep_end - time(NULL));
            char b[64];
            snprintf(b, sizeof(b), "Sleep timer: %dm left", rem > 0 ? (rem + 59) / 60 : 0);
            txt = b;
        }
    } else if (g_suspend_at) {
        int rem = (int)(g_suspend_at - time(NULL));
        char b[64];
        snprintf(b, sizeof(b), "Suspending in %dm unless you press play", rem > 0 ? (rem + 59) / 60 : 0);
        txt = b;
    }
    ui_label_set(g_sleep_badge, txt);
}

void app_set_sleep_mode(int index) {
    if (index < 0 || index >= kSleepOptionCount) index = 0;
    g_sleep_mode = index;
    if (kSleepOptions[index].minutes > 0)
        g_sleep_end = time(NULL) + kSleepOptions[index].minutes * 60;
    g_suspend_at = 0;
    update_sleep_badge();
}

bool app_refresh_disabled() { return !dispUpdate; }
void app_set_refresh_disabled(bool disabled) {
    dispUpdate = !disabled;
    db.setSetting("disable_refresh", disabled ? "1" : "0");
}

bool app_backlight_disabled() { return db.getSetting("disable_backlight", "0") == "1"; }
void app_set_backlight_disabled(bool disabled) {
    db.setSetting("disable_backlight", disabled ? "1" : "0");
    if (!lipcInstance) return;
    if (disabled) {
        int cur = 0;
        LipcGetIntProperty(lipcInstance, "com.lab126.powerd", "flIntensity", &cur);
        if (cur > 0) flIntensity = cur;   // remember it so it can be restored
        LipcSetIntProperty(lipcInstance, "com.lab126.powerd", "flIntensity", 0);
    } else if (flIntensity > 0) {
        LipcSetIntProperty(lipcInstance, "com.lab126.powerd", "flIntensity", flIntensity);
    }
}

bool app_auto_suspend() { return db.getSetting("auto_suspend", "0") == "1"; }
void app_set_auto_suspend(bool enabled) {
    db.setSetting("auto_suspend", enabled ? "1" : "0");
    if (!enabled) {
        g_suspend_at = 0;
        g_suspend_requested_at = 0;
        if (g_suspend_released) {
            disableSleep();
            g_suspend_released = false;
        }
        update_sleep_badge();
    }
}

void app_open_bluetooth_settings() {
    LipcSetStringProperty(lipcInstance,"com.lab126.btfd","BTenable","1:1");
    LipcSetStringProperty(lipcInstance,"com.lab126.pillow","customDialog","{\"name\":\"bt_wizard_dialog\", \"clientParams\": {\"show\":true, \"winmgrModal\":true, \"replySrc\":\"\"}}");
}

// ---------------------------------------------------------------------------------
// E-ink refresh scheduling
// ---------------------------------------------------------------------------------

static guint g_refresh_src = 0;
static bool  g_refresh_full = false;

static gboolean refresh_fire(gpointer) {
    g_refresh_src = 0;
    if (g_refresh_full) eink_full_refresh(); else eink_soft_refresh();
    g_refresh_full = false;
    return FALSE;
}

// The refresh must come after GTK flushed its repaint to the X server, so it
// is deferred a little instead of being issued from the tap handler.
void app_schedule_refresh(bool full) {
    if (full) g_refresh_full = true;
    if (g_refresh_src) return;
    g_refresh_src = g_timeout_add(180, refresh_fire, NULL);
}

// ---------------------------------------------------------------------------------
// Player screen: cover / title toggle, info lines
// ---------------------------------------------------------------------------------

static bool show_title_mode() {
    return db.getSetting("show_title", "0") == "1";
}

void player_apply_display_mode() {
    if (show_title_mode()) {
        gtk_widget_hide(cover_image);
        gtk_widget_show(g_title_block);
    } else {
        gtk_widget_show(cover_image);
        gtk_widget_hide(g_title_block);
    }
}

static gboolean on_cover_pressed(GtkWidget*, GdkEventButton*, gpointer) {
    db.setSetting("show_title", show_title_mode() ? "0" : "1");
    player_apply_display_mode();
    app_schedule_refresh(true);
    return TRUE;
}

void player_set_cover(GdkPixbuf* pix) {
    GdkPixbuf* src = pix;
    GdkPixbuf* placeholder = NULL;
    if (!src) {
        placeholder = gdk_pixbuf_new_from_inline(-1, audiobook, FALSE, NULL);
        src = placeholder;
    }
    if (!src) { gtk_image_clear(GTK_IMAGE(cover_image)); return; }
    int pw = gdk_pixbuf_get_width(src), ph = gdk_pixbuf_get_height(src);
    double maxw = ui_px(420), maxh = ui_px(430);
    double sc = (pw > 0 && ph > 0) ? MIN(maxw / pw, maxh / ph) : 1.0;
    if (!pix) sc *= 0.8;   // generic icon: a bit smaller than real artwork
    int nw = MAX(1, (int)(pw * sc)), nh = MAX(1, (int)(ph * sc));
    GdkPixbuf* scaled = gdk_pixbuf_scale_simple(src, nw, nh, GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(cover_image), scaled);
    g_object_unref(scaled);
    if (placeholder) g_object_unref(placeholder);
}

void player_set_book(const std::string& title, const std::string& author) {
    ui_label_set(title_label, title);
    ui_label_set(artist_label, author);
}

void player_set_info(const std::string& chapter, const std::string& time,
                     const std::string& left) {
    ui_label_set(chapter_label, chapter);
    ui_label_set(time_label, time.empty() ? " " : time);
    ui_label_set(left_label, left.empty() ? " " : left);
}

// ---------------------------------------------------------------------------------
// Local file helpers
// ---------------------------------------------------------------------------------

std::string get_home_dir() {
    return "/mnt/us";
}

std::string get_db_path() {
    return get_home_dir() + "/.lark_player.db";
}

void save_history() {
    if (abs_is_active()) return;   // ABS progress is synced to the server, not the local DB
    if (!current_file.empty()) {
        gint64 pos = backend.get_position() / GST_SECOND;
        gint64 dur = backend.get_duration() / GST_SECOND;
        // If duration is 0 (e.g. stopped), try to get it from book if we can, or just pass 0.
        // updateBookProgress handles insert/update.
        db.updateBookProgress(current_file, (int)pos, (int)dur, false);
        db.setSetting("last_opened_file", current_file);
    }
}

void init_database() {
    std::string db_path = get_db_path();
    if (!db.init(db_path)) {
        g_print("Failed to initialize database at %s\n", db_path.c_str());
        return;
    }

    // Check for legacy history and migrate
    std::string legacy_path = get_home_dir() + "/.lark_history";
    std::ifstream legacy_file(legacy_path);
    if (legacy_file.good()) {
        legacy_file.close();
        g_print("Migrating legacy history from %s\n", legacy_path.c_str());
        if (db.migrateFromLegacy(legacy_path)) {
            g_print("Migration successful.\n");
        } else {
            g_print("Migration failed.\n");
        }
    }

    // Restore last opened file
    std::string last_file = db.getLastPlayedFile();
    if (!last_file.empty() && current_file.empty()) {
        current_file = last_file;
    }
}

// Basename without extension, used when the file has no embedded tags (MP3/FLAC/WAV)
std::string get_display_name(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

// Chapter of the local book containing pos (seconds), or -1.
static int local_chapter_at(double pos) {
    int idx = -1;
    for (size_t i = 0; i < backend.chapters.size(); i++) {
        if (backend.chapters[i].timestamp / 10000000.0 <= pos + 0.001) idx = (int)i;
    }
    return idx;
}

// Seconds until the end of the current chapter of the local book, or -1.
static double local_seconds_to_chapter_end() {
    if (!(backend.is_playing || backend.is_paused) || backend.chapters.empty()) return -1;
    double pos = backend.get_position() / (double)GST_SECOND;
    int idx = local_chapter_at(pos);
    if (idx < 0) return -1;
    double end = (size_t)(idx + 1) < backend.chapters.size()
                     ? backend.chapters[idx + 1].timestamp / 10000000.0
                     : backend.get_duration() / (double)GST_SECOND;
    return end - pos;
}

void update_metadata_ui() {
    std::string title = backend.meta_title;
    if (title.empty() && !current_file.empty()) {
        title = get_display_name(current_file);
    }
    player_set_book(title, backend.meta_artist);

    if (!backend.cover_art.empty()) {
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
        gdk_pixbuf_loader_write(loader, backend.cover_art.data(), backend.cover_art.size(), NULL);
        gdk_pixbuf_loader_close(loader, NULL);
        GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        player_set_cover(pixbuf);   // NULL (undecodable art) falls back to the icon
        g_object_unref(loader);
    } else {
        player_set_cover(NULL);
    }
    player_apply_display_mode();
}

gboolean show_error_dialog_idle(gpointer data) {
    char *msg = (char*)data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                       GTK_MESSAGE_ERROR,
                                       GTK_BUTTONS_OK,
                                       "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dialog), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(msg);
    return FALSE;
}

static gboolean stop_backend_and_show_error_idle(gpointer data) {
    backend.stop();
    return show_error_dialog_idle(data);
}

// Called from the decoder thread; GTK calls must happen on the main loop
void on_backend_error(const char* msg, void* data) {
    (void)data;
    g_idle_add(stop_backend_and_show_error_idle, g_strdup(msg));
}

static void show_info_dialog(const char* msg) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                       GTK_MESSAGE_INFO,
                                       GTK_BUTTONS_OK,
                                       "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dialog), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static bool local_file_is_readable(const char* filepath) {
    if (!filepath || !*filepath) return false;
    struct stat st;
    return stat(filepath, &st) == 0 && S_ISREG(st.st_mode) &&
           access(filepath, R_OK) == 0;
}

static void save_current_local_progress() {
    if (current_file.empty() || (!backend.is_playing && !backend.is_paused)) return;
    int pos = (int)(backend.get_position() / GST_SECOND);
    int dur = 0;
    Book saved;
    if (db.getBookProgress(current_file, saved)) dur = saved.duration;
    // Querying the active pipeline for duration can block after the backing
    // file has been removed. Keep the last known duration in that case.
    if (local_file_is_readable(current_file.c_str()))
        dur = (int)(backend.get_duration() / GST_SECOND);
    db.updateBookProgress(current_file, pos, dur, false);
}

static void handle_missing_local_file() {
    if (current_file.empty() || abs_is_active() ||
        local_file_is_readable(current_file.c_str())) return;

    save_current_local_progress();
    backend.stop();
    current_file.clear();
    last_timestamp = 0;
    db.setSetting("last_opened_file", "");
    player_set_book("Local file unavailable", "");
    player_set_cover(NULL);
    player_set_info("The audiobook was deleted or moved.", "", "");
    ui_glyph_set_icon(g_play_glyph, UI_ICON_PLAY);
    app_schedule_refresh(false);
    show_info_dialog("The local audiobook was deleted or moved.");
}

// ---------------------------------------------------------------------------------
// Kindle Quick Settings / framework overlay recovery.
// The stock Quick Settings drawer paints over our framebuffer without going
// through X, so when it closes GTK thinks its window is intact and the covered
// region stays stale. We schedule one debounced no-flash full-screen e-ink
// refresh on the next tap or expose so the covered area is repainted.
// ---------------------------------------------------------------------------------

static guint screen_restore_src = 0;

static gboolean screen_restore_fire(gpointer data) {
    (void)data;
    screen_restore_src = 0;
    eink_soft_refresh();
    return FALSE;   // one-shot
}

static void schedule_screen_restore() {
    if (screen_restore_src) return;   // already pending
    screen_restore_src = g_timeout_add(250, screen_restore_fire, NULL);
}

static gboolean on_window_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    (void)widget; (void)event; (void)data;
    schedule_screen_restore();
    return FALSE;   // let normal handling continue
}

// Generic hook for framework-overlay recovery: any signal that the window was
// hidden/unfocused/reshaped by the Kindle framework (Quick Settings drawer,
// screensaver, dialog overlays) schedules a debounced no-flash repaint.
static gboolean on_window_overlay_event(GtkWidget* widget, gpointer event, gpointer data) {
    (void)widget; (void)event; (void)data;
    schedule_screen_restore();
    return FALSE;   // let normal handling continue
}

static gboolean on_window_expose(GtkWidget* widget, GdkEventExpose* event, gpointer data) {
    (void)widget; (void)event; (void)data;
    schedule_screen_restore();
    return FALSE;   // let GTK repaint the exposed region
}

// The app losing focus (Quick Settings drawer, the Kindle home screen, a
// screensaver) is exactly the "leave the book" moment users notice progress
// going stale in: flush the position now rather than hoping the next 15s
// tick still gets to run before the app is frozen/backgrounded.
static gboolean on_window_focus_out(GtkWidget* widget, GdkEventFocus* event, gpointer data) {
    (void)widget; (void)event; (void)data;
    if (abs_is_active()) abs_force_sync();
    return FALSE;
}

// The Kindle status bar (clock, wifi, battery) overlays the top of the app
// window. Reserve room for it unless the window manager already placed the
// window below the bar.
static gboolean on_window_map(GtkWidget* widget, gpointer event, gpointer data) {
    (void)event; (void)data;
    gint ox = 0, oy = 0;
    GdkWindow* gw = gtk_widget_get_window(widget);
    if (gw) gdk_window_get_origin(gw, &ox, &oy);
    int want = ui_px(66);
    ui_set_status_inset(oy >= want ? 0 : want - oy);
    return FALSE;
}

// ---------------------------------------------------------------------------------
// Playback control + periodic UI tick
// ---------------------------------------------------------------------------------

static bool playing_now() {
    return backend.is_playing && !backend.is_paused;
}

static double seconds_to_chapter_end() {
    return abs_is_active() ? abs_seconds_to_chapter_end() : local_seconds_to_chapter_end();
}

static void update_local_info() {
    if (!backend.is_playing && !backend.is_paused) return;

    double pos = backend.get_position() / (double)GST_SECOND;
    double len = backend.get_duration() / (double)GST_SECOND;
    std::string chapter, left;

    int idx = local_chapter_at(pos);
    if (idx >= 0) {
        chapter = backend.chapters[idx].title;
        double end = (size_t)(idx + 1) < backend.chapters.size()
                         ? backend.chapters[idx + 1].timestamp / 10000000.0 : len;
        left = fmt_left_text(end - pos, "chapter");
    } else if (len > 0) {
        left = fmt_left_text(len - pos, "book");
    }

    if (dispUpdate)
        player_set_info(chapter, fmt_clock(pos) + " / " + fmt_clock(len), left);
    else
        player_set_info(chapter, "", "");
}

gboolean update_ui(gpointer data) {
    (void)data;
    handle_missing_local_file();
    abs_sync_tick();   // ABS: throttled server progress sync (no-op when inactive)

    bool playing = playing_now();

    // Sleep timer
    if (g_sleep_mode > 0) {
        const SleepOption& opt = kSleepOptions[g_sleep_mode];
        bool fire = false;
        if (opt.minutes > 0) {
            fire = (time(NULL) >= g_sleep_end);
        } else if (opt.minutes < 0 && playing) {
            double secs = seconds_to_chapter_end();
            fire = (secs >= 0.0 && secs < 1.5);
        }
        if (fire) {
            g_sleep_mode = 0;
            if (playing) backend.pause();
            if (app_auto_suspend()) g_suspend_at = time(NULL) + AUTO_SUSPEND_SECS;
            eink_full_refresh();
        }
    }

    // Auto suspend: suspend the Kindle, then restore sleep prevention
    // when powerd reports that the device has woken up.
    if (g_suspend_at || g_suspend_released) {
        time_t now = time(NULL);

        if (playing) {
            g_suspend_at = 0;
            g_suspend_requested_at = 0;
            if (g_suspend_released) {
                disableSleep();
                g_suspend_released = false;
            }
        } else if (g_suspend_at && now >= g_suspend_at) {
            g_print("Auto suspend: requesting Kindle suspend\n");

            enableSleep();
            LIPCcode rc = LipcSetIntProperty(
                lipcInstance, "com.lab126.powerd", "powerButton", 1);

            if (rc == LIPC_OK) {
                g_suspend_released = true;
                g_suspend_requested_at = now;
                g_print("Auto suspend: powerButton request sent\n");
            } else {
                g_print("Auto suspend: powerButton request failed (%d)\n",
                        (int)rc);
                disableSleep();
                g_suspend_released = false;
                g_suspend_requested_at = 0;
            }

            g_suspend_at = 0;
        } else if (g_suspend_released && g_suspend_requested_at &&
                   now >= g_suspend_requested_at + 5) {
            char *power_state = NULL;
            LIPCcode rc = LipcGetStringProperty(
                lipcInstance, "com.lab126.powerd", "state", &power_state);

            if (rc == LIPC_OK && power_state) {
                bool awake = (strcmp(power_state, "active") == 0);
                LipcFreeString(power_state);

                if (awake) {
                    g_print("Auto suspend: Kindle woke; restoring sleep prevention\n");
                    disableSleep();
                    g_suspend_released = false;
                    g_suspend_requested_at = 0;
                }
            } else if (power_state) {
                LipcFreeString(power_state);
            }
        }
    }
    update_sleep_badge();

    ui_glyph_set_icon(g_play_glyph, playing ? UI_ICON_PAUSE : UI_ICON_PLAY);

    if (abs_is_active()) {
        abs_time_display(dispUpdate);   // ABS: chapter / time / remaining lines
        return TRUE;
    }
    update_local_info();
    return TRUE;
}

void on_play_pause_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;(void)data;
    bool was_playing = backend.is_playing && !backend.is_paused;
    if (backend.is_playing) {
        backend.pause();
    } else if (backend.is_paused) {
        backend.pause();
    } else if (abs_is_active()) {
        // ABS book was stopped (e.g. EOS of last track, or error): replay current position
        abs_replay_current();
    } else {
        if (!current_file.empty()) {
            backend.play_file(current_file.c_str(), last_timestamp);
        }
    }
    // Pausing: push the position to the server right away rather than waiting
    // for the next periodic tick, so another Audiobookshelf client sees it
    // immediately instead of a few seconds late.
    if (was_playing && backend.is_paused && abs_is_active()) abs_force_sync();
    ui_glyph_set_icon(g_play_glyph, playing_now() ? UI_ICON_PAUSE : UI_ICON_PLAY);
}

void jump_relative(int seconds) {
    if (abs_is_active()) {
        abs_jump_relative(seconds);
        return;
    }
    if (current_file.empty()) return;

    gint64 duration = backend.get_duration() / GST_SECOND;

    int current_pos = last_timestamp;
    if (backend.is_playing || backend.is_paused) {
        current_pos = backend.get_position() / GST_SECOND;
    }

    int new_pos = current_pos + seconds;
    if (new_pos < 0) new_pos = 0;
    if (duration > 0 && new_pos > duration) new_pos = duration;

    last_timestamp = new_pos;

    backend.play_file(current_file.c_str(), new_pos);
    update_local_info();
    app_schedule_refresh(false);
}

static void on_rewind_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;(void)data;
    jump_relative(-10);
}

static void on_ff_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;(void)data;
    jump_relative(10);
}

static void on_avrcp_command(GDBusConnection*, const gchar*, const gchar*,
                             const gchar*, const gchar*, GVariant* parameters,
                             gpointer) {
    // Kindle emits the Bluetooth media-key command on the system bus. Route
    // it through the app's playback controls instead of synthesizing a screen
    // tap at a firmware-specific coordinate.
    gint64 now = g_get_monotonic_time();
    if (now - avrcp_last_command_us < 500000) return;
    avrcp_last_command_us = now;

    gchar* printed = parameters ? g_variant_print(parameters, TRUE) : g_strdup("");
    std::string command = printed ? printed : "";
    g_free(printed);
    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    bool playing = backend.is_playing && !backend.is_paused;
    if (command.find("pause") != std::string::npos) {
        if (playing) on_play_pause_clicked(g_play_glyph, NULL);
    } else if (command.find("play") != std::string::npos) {
        if (!playing) on_play_pause_clicked(g_play_glyph, NULL);
    } else {
        // Firmware versions that omit the command argument only send this
        // signal for the play/pause button.
        on_play_pause_clicked(g_play_glyph, NULL);
    }
}

static void start_avrcp_listener() {
    GError* error = NULL;
    avrcp_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!avrcp_bus) {
        g_warning("Could not connect to the Bluetooth command bus: %s",
                  error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return;
    }
    g_dbus_connection_signal_subscribe(
        avrcp_bus, NULL, "com.lab126.btfd", "BTavrcpCommand", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_avrcp_command, NULL, NULL);
}

void on_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    // Final ABS progress sync runs on a background thread; the UI thread
    // never touches the network, so the Exit button always responds.
    bool had_abs_session = abs_is_active();
    abs_begin_shutdown_sync();
    backend.stop();
    if (!had_abs_session) save_history();
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",flIntensity);
    LipcSetIntProperty(lipcInstance,"com.lab126.btfd","ensureBTconnection",0);
    enableSleep();
    closeLipcInstance();
    gtk_main_quit();
    // Give the final sync a bounded window to reach the server (2s), then
    // exit regardless - close must never hang.
    abs_wait_shutdown_sync(2000);
    exit(0);
}

void app_exit() {
    g_print("UI: exit button clicked\n");
    on_destroy(NULL, NULL);
}

void on_file_open(const char* filepath) {
    if (!filepath) return;

    if (!local_file_is_readable(filepath)) {
        if (current_file == filepath && !abs_is_active())
            handle_missing_local_file();
        else
            show_info_dialog("This audiobook file is missing or cannot be read.");
        return;
    }

    if (abs_is_active()) abs_shutdown();   // opening a local file ends any ABS session

    save_current_local_progress();

    backend.stop();

    current_file = filepath;
    db.setSetting("last_opened_file", current_file);

    Book book;
    if (db.getBookProgress(filepath, book)) {
        last_timestamp = book.last_position;
    } else {
        last_timestamp = 0;
    }

    g_print("Reading metadata for %s\n", filepath);
    backend.read_metadata(filepath);
    g_print("Metadata read: Title='%s', Artist='%s', Album='%s'\n",
            backend.meta_title.c_str(),
            backend.meta_artist.c_str(),
            backend.meta_album.c_str());
    update_metadata_ui();
    g_print("Starting playback for %s at %d seconds\n", filepath, last_timestamp);
    backend.play_file(filepath, last_timestamp);
}

void app_open_local_file(const char* path) {
    on_file_open(path);
    app_show_tab(TAB_PLAY);
}

void app_open_file_dialog() {
    GtkWidget *dialog;
    dialog = gtk_file_chooser_dialog_new("L:A_N:application_PC:T_ID:com.kbarni.larkplayer",
                                         GTK_WINDOW(window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                         GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                         NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename;
        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        last_timestamp = 0;
        gtk_widget_destroy(dialog);
        dialog = NULL;
        app_open_local_file(filename);
        g_free(filename);
    }
    if (dialog) gtk_widget_destroy(dialog);
}

// ---------------------------------------------------------------------------------
// Bookmarks and chapters (local books)
// ---------------------------------------------------------------------------------

static void on_add_bookmark_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;(void)data;
    if (abs_is_active()) {
        show_info_dialog("Bookmarks are not available for Audiobookshelf books yet.");
        return;
    }
    if (current_file.empty()) return;

    bool was_playing = backend.is_playing && !backend.is_paused;
    if (was_playing) {
        backend.pause();
    }

    int pos = backend.get_position() / GST_SECOND;
    std::string file = current_file;

    std::vector<Bookmark> bookmarks = db.getBookmarks(file);
    char buf[80];
    snprintf(buf, sizeof(buf), "Bookmark %d", (int)bookmarks.size() + 1);

    app_keyboard("Bookmark name", buf, false,
        [file, pos, was_playing](bool ok, const std::string& text) {
            if (ok && !text.empty()) {
                char name[600];
                snprintf(name, sizeof(name), "%s %02d:%02d:%02d", text.c_str(),
                         pos / 3600, (pos % 3600) / 60, pos % 60);
                if (db.addBookmark(file, pos, name))
                    g_print("Bookmark added: %s\n", text.c_str());
            }
            if (was_playing && backend.is_paused) backend.pause();
        });
}

static void on_bookmark_list_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;(void)data;
    if (abs_is_active()) {
        show_info_dialog("Bookmarks are not available for Audiobookshelf books yet.");
        return;
    }
    if (current_file.empty()) return;

    std::vector<Bookmark> bookmarks = db.getBookmarks(current_file);
    if (bookmarks.empty()) {
        show_info_dialog("No bookmarks yet.\nUse \"+ Bookmark\" to add one.");
        return;
    }
    std::vector<std::string> rows, rights;
    for (const auto& b : bookmarks) {
        rows.push_back(b.name);
        rights.push_back("");
    }
    int pick = ui_pick_dialog(GTK_WINDOW(window), "Bookmarks", rows, rights, -1);
    if (pick >= 0 && pick < (int)bookmarks.size()) {
        last_timestamp = bookmarks[pick].position;
        backend.play_file(current_file.c_str(), last_timestamp);
    }
}

static void on_chapter_list_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;(void)data;

    if (abs_is_active()) {
        abs_show_chapter_list();   // ABS books: server chapter metadata
        return;
    }

    if (backend.chapters.empty()) {
        show_info_dialog("No chapters found.");
        return;
    }

    double pos = backend.get_position() / (double)GST_SECOND;
    std::vector<std::string> rows, rights;
    for (const auto& ch : backend.chapters) {
        rows.push_back(ch.title);
        rights.push_back(fmt_clock(ch.timestamp / 10000000.0));
    }
    int pick = ui_pick_dialog(GTK_WINDOW(window), "Chapters", rows, rights, local_chapter_at(pos));
    if (pick >= 0 && pick < (int)backend.chapters.size()) {
        last_timestamp = (int)(backend.chapters[pick].timestamp / 10000000);
        backend.play_file(current_file.c_str(), last_timestamp);
    }
}

// ---------------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------------

void app_show_tab(int tab) {
    if (!g_notebook) return;
    if (tab < TAB_PLAY || tab > TAB_OPTIONS) tab = TAB_PLAY;
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    // Leaving the book behind (literally, per the Library/Options tabs): sync
    // its position right away instead of leaving it for the next tick.
    if (cur == TAB_PLAY && tab != TAB_PLAY && abs_is_active()) abs_force_sync();
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), tab);
    ui_tabbar_set_active(g_tabbar, tab);
    if (tab == TAB_LIBRARY) lib_page_activate();
    if (tab == TAB_OPTIONS) options_page_refresh();
    if (tab != cur) app_schedule_refresh(true);
}

// The keyboard is page 3 of the notebook; the tab bar is hidden while it is up.
static int g_kb_prev_page = TAB_PLAY;

void app_set_keyboard_visible(bool visible) {
    if (!g_notebook) return;
    if (visible) {
        g_kb_prev_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), 3);
        gtk_widget_hide(g_tabbar);
    } else {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), g_kb_prev_page);
        gtk_widget_show(g_tabbar);
    }
    app_schedule_refresh(true);
}

static void on_tab_tapped(GtkWidget*, gpointer idx) {
    app_show_tab(GPOINTER_TO_INT(idx));
}

// ---------------------------------------------------------------------------------
// Player page (canva mockup 1)
//
// Vertical layout in mockup px (1072 x 1340 above the tab bar). Every element
// sits in a fixed-height slot, so text changes never move anything.
// ---------------------------------------------------------------------------------

static void add_slot(GtkWidget* box, GtkWidget* child, int mock_h, double xalign = 0.5) {
    GtkWidget* a = gtk_alignment_new((gfloat)xalign, 0.5f, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), child);
    gtk_widget_set_size_request(a, -1, ui_px(mock_h));
    gtk_box_pack_start(GTK_BOX(box), a, FALSE, FALSE, 0);
}

static GtkWidget* player_page_new() {
    GtkWidget* page = gtk_vbox_new(FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), ui_inset_new(), FALSE, FALSE, 0);          // status bar

    // Heading
    // The heading and a filler at the bottom absorb whatever height the window
    // has beyond the fixed layout (the Kindle gives the app less than the full
    // screen), so nothing below is ever cut off.
    GtkWidget* heading = ui_label_new("LARK-Next", 80, true);
    GtkWidget* heading_slot = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(heading_slot), heading);
    gtk_widget_set_size_request(heading_slot, -1, ui_px(120));
    gtk_box_pack_start(GTK_BOX(page), heading_slot, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(page), ui_vspace(20), FALSE, FALSE, 0);

    // Cover (tap toggles cover <-> title/author)
    cover_image = gtk_image_new();
    title_label = ui_label_new("", 46, true, false, 0.5, COVER_BOX_W - 20, 26, 5);
    artist_label = ui_label_new("", 32, false, true, 0.5, COVER_BOX_W - 20, 51, 2);
    g_title_block = gtk_vbox_new(FALSE, ui_px(14));
    gtk_box_pack_start(GTK_BOX(g_title_block), title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_title_block), artist_label, FALSE, FALSE, 0);

    GtkWidget* cover_inner = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cover_inner), cover_image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cover_inner), g_title_block, FALSE, FALSE, 0);
    GtkWidget* cover_center = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(cover_center), cover_inner);
    GtkWidget* cover_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(cover_box), cover_center);
    gtk_widget_set_size_request(cover_box, ui_px(COVER_BOX_W), ui_px(COVER_BOX_H));
    g_signal_connect(cover_box, "button-press-event", G_CALLBACK(on_cover_pressed), NULL);
    add_slot(page, cover_box, COVER_BOX_H);

    gtk_box_pack_start(GTK_BOX(page), ui_vspace(80), FALSE, FALSE, 0);

    // Transport: rewind / play-pause / fast-forward
    GtkWidget* transport = gtk_hbox_new(TRUE, 0);
    GtkWidget* rw = ui_glyph_button_new(UI_ICON_REWIND, 130, 230, 180);
    ui_connect(rw, on_rewind_clicked, NULL);
    g_play_glyph = ui_glyph_button_new(UI_ICON_PLAY, 130, 230, 180);
    ui_connect(g_play_glyph, on_play_pause_clicked, NULL);
    GtkWidget* ff = ui_glyph_button_new(UI_ICON_FORWARD, 130, 230, 180);
    ui_connect(ff, on_ff_clicked, NULL);
    gtk_box_pack_start(GTK_BOX(transport), rw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(transport), g_play_glyph, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(transport), ff, TRUE, TRUE, 0);
    add_slot(page, transport, 180);

    gtk_box_pack_start(GTK_BOX(page), ui_vspace(55), FALSE, FALSE, 0);

    // Chapter + time lines
    chapter_label = ui_label_new("Open the Library to pick a book", 28, false, false, 0.5, 960, 26, 1);
    add_slot(page, chapter_label, 65);
    gtk_box_pack_start(GTK_BOX(page), ui_vspace(20), FALSE, FALSE, 0);
    time_label = ui_label_new("00:00:00 / 00:00:00", 28);
    add_slot(page, time_label, 36);
    left_label = ui_label_new(" ", 28);
    add_slot(page, left_label, 34);

    gtk_box_pack_start(GTK_BOX(page), ui_vspace(34), FALSE, FALSE, 0);

    // Chapters / bookmarks (existing features, fitted under the time info)
    GtkWidget* extras = gtk_hbox_new(FALSE, ui_px(24));
    GtkWidget* b_ch = ui_button_new("Chapters", 28, 270, 72, 3, 8);
    ui_connect(b_ch, on_chapter_list_clicked, NULL);
    GtkWidget* b_bl = ui_button_new("Bookmarks", 28, 270, 72, 3, 8);
    ui_connect(b_bl, on_bookmark_list_clicked, NULL);
    GtkWidget* b_ba = ui_button_new("+ Bookmark", 28, 270, 72, 3, 8);
    ui_connect(b_ba, on_add_bookmark_clicked, NULL);
    gtk_box_pack_start(GTK_BOX(extras), b_ch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(extras), b_bl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(extras), b_ba, FALSE, FALSE, 0);
    add_slot(page, extras, 72);

    g_sleep_badge = ui_label_new("", 24, false, true, 0.5, 900, 77, 1);
    add_slot(page, g_sleep_badge, 44);
    gtk_box_pack_start(GTK_BOX(page), ui_vspace(1), TRUE, TRUE, 0);

    dump_alloc("chapters_btn", b_ch);
    dump_alloc("bookmarks_btn", b_bl);
    dump_alloc("add_bookmark_btn", b_ba);
    dump_alloc("rewind", rw);
    dump_alloc("play_pause", g_play_glyph);
    dump_alloc("forward", ff);
    return page;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered g_print: survives crashes
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    gtk_init(&argc, &argv);

    ui_install_theme();

    backend.set_error_callback(on_backend_error, NULL);
    backend.set_eos_callback(abs_on_eos_wrapper, NULL);   // ABS: track chaining

    init_database();
    abs_reconcile_stale_session();   // finalize anything left open by a crash/kill
    if (argc > 1) {
        current_file = argv[1];
        last_timestamp = 0;
    }

    int width, height;
    ui_screen_size(&width, &height);
    is_hires = (width >= 1000);
    dispUpdate = db.getSetting("disable_refresh", "0") != "1";

    g_print("Detected resolution: %dx%d, using %s mode\n", width, height, (is_hires?"High res":"Low res"));

    openLipcInstance();
    disableSleep();
    LipcGetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",&flIntensity);
    if (app_backlight_disabled())
        app_set_backlight_disabled(true);

    LipcSetIntProperty(lipcInstance,"com.lab126.btfd","ensureBTconnection",1);
    LipcSetStringProperty(lipcInstance,"com.lab126.btfd","BTenable","1:1");

    // Window Setup
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_size_request(window, DESKTOP_W_SIZE, DESKTOP_H_SIZE);
    if (getenv("LARK_WINDOW_SIZE")) {
        // Preview/testing: give the window the full "screen" size
        gtk_widget_set_size_request(window, width, height);
        gtk_window_set_default_size(GTK_WINDOW(window), width, height);
        gtk_window_move(GTK_WINDOW(window), 0, 0);
    }
    gtk_window_set_title(GTK_WINDOW(window), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    // Quick Settings / framework overlay recovery (see on_window_button_press).
    // The QS drawer can cover the app without any X expose, so watch focus
    // changes, visibility, map/unmap and button presses as recovery triggers.
    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_VISIBILITY_NOTIFY_MASK |
                                  GDK_FOCUS_CHANGE_MASK | GDK_STRUCTURE_MASK);
    g_signal_connect(window, "button-press-event", G_CALLBACK(on_window_button_press), NULL);
    g_signal_connect(window, "expose-event", G_CALLBACK(on_window_expose), NULL);
    g_signal_connect(window, "focus-in-event", G_CALLBACK(on_window_overlay_event), NULL);
    g_signal_connect(window, "focus-out-event", G_CALLBACK(on_window_focus_out), NULL);
    g_signal_connect(window, "map-event", G_CALLBACK(on_window_overlay_event), NULL);
    g_signal_connect(window, "map-event", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(window, "visibility-notify-event", G_CALLBACK(on_window_overlay_event), NULL);

    GtkRcStyle *style = gtk_widget_get_modifier_style(window);
    style->bg[GTK_STATE_NORMAL].red = 65535;
    style->bg[GTK_STATE_NORMAL].green = 65535;
    style->bg[GTK_STATE_NORMAL].blue = 65535;
    gtk_widget_modify_style(window, style);

    // Three screens (Player / Library / Options) behind one bottom tab bar
    GtkWidget *main_vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    g_notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(g_notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(g_notebook), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), player_page_new(), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), lib_page_new(), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), options_page_new(), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), keyboard_page_new(), NULL);
    gtk_box_pack_start(GTK_BOX(main_vbox), g_notebook, TRUE, TRUE, 0);

    g_tabbar = ui_tabbar_new(108, on_tab_tapped);
    gtk_box_pack_start(GTK_BOX(main_vbox), g_tabbar, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), TAB_PLAY);
    ui_tabbar_set_active(g_tabbar, TAB_PLAY);

    player_set_cover(NULL);
    player_set_book("No book selected", "");
    player_apply_display_mode();
    options_page_refresh();

    dump_alloc("tab_bar", g_tabbar);
    dump_alloc("cover", cover_image);
    dump_alloc("chapter_label", chapter_label);
    dump_alloc("time_label", time_label);

    if (!current_file.empty()) {
        on_file_open(current_file.c_str());
    }

    start_avrcp_listener();
    g_timeout_add(1000, update_ui, NULL);
    abs_debug_autostart();   // LARK_ABS_TEST=1: auto-open the library for testing

    gtk_main();

    return 0;
}
