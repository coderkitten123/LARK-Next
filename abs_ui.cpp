// =================================================================================
// abs_ui.cpp - Stage 2 (library browser UI) + Stage 3-lite (ABS playback glue)
//
// Flows:
//   open_abs_browser()      : config check -> library window (list, covers, search, pages)
//   row click               : item detail -> /play session -> download track -> play
//   EOS / app close         : sync progress, chain tracks, close session
//
// Playback uses download-to-tmp for the selected track (LAN-fast, enables exact
// resume + seeking), then chains on EOS. Pure streaming (StreamVFS) is a Stage 3
// optimization for track starts.
// =================================================================================

#include <gtk/gtk.h>
#include <glib.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

#include "abs_client.h"
#include "abs_ui.h"
#include "database_manager.h"
#include "music_backend.h"

// declared (not defined) here: keyboarddialog.hpp defines the function inline,
// so including it here would duplicate symbols with m4b_player.cpp.
void show_dialog_keyboard(std::string& text);

// --- externs to m4b_player.cpp globals ---
extern MusicBackend backend;
extern DatabaseManager db;
extern GtkWidget *window;
extern GtkWidget *title_label;
extern GtkWidget *artist_label;
extern GtkWidget *cover_image;
extern bool dispUpdate;
extern bool is_hires;
extern int last_timestamp;

// Local file with no ABS context - referenced so we can clear the "current file"
// state safely. Defined in m4b_player.cpp.
extern std::string current_file;

AbsClient gAbs;

// ---------------------------------------------------------------------------------
// ABS playback state
// ---------------------------------------------------------------------------------

static const char* ABS_CACHE_DIR = "/tmp/abs_cache";
static const char* ABS_COVER_DIR = "/mnt/us/abs_covers";
static const int   ABS_PAGE_SIZE = 8;
static const int   ABS_SYNC_INTERVAL = 30;   // seconds between /sync calls

struct AbsPlayback {
    bool active = false;
    std::string session_id;
    std::string item_id;
    std::string book_title;
    std::vector<AbsTrack> tracks;
    int track_idx = -1;                 // current track (0-based into tracks)
    double book_position = 0;           // seconds into the book
    double book_duration = 0;
    double last_sync_position = 0;      // book position at last /sync
    int last_sync_tick = 0;             // updated once per abs_sync_tick call
};
static AbsPlayback ap;

bool abs_is_active() { return ap.active; }

// Backend EOS callback adapter (EosCallback signature)
void abs_on_eos_wrapper(void* user_data) {
    (void)user_data;
    abs_on_eos();
}

// ---------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------

static void set_player_labels(const std::string& title, const std::string& subtitle) {
    char* markup = g_markup_printf_escaped("<span font_desc='Sans Bold %d'>%s</span>",
                                           is_hires ? 24 : 18, title.c_str());
    gtk_label_set_markup(GTK_LABEL(title_label), markup);
    g_free(markup);
    markup = g_markup_printf_escaped("<span font_desc='Sans Italic %d'>%s</span>",
                                     is_hires ? 14 : 12, subtitle.c_str());
    gtk_label_set_markup(GTK_LABEL(artist_label), markup);
    g_free(markup);
}

static std::string fmt_time(double secs) {
    int s = (int)secs;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
    return buf;
}

static void show_abs_error(const std::string& msg) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(window),
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                          "%s", msg.c_str());
    gtk_window_set_title(GTK_WINDOW(d), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

// ---------------------------------------------------------------------------------
// Playback glue
// ---------------------------------------------------------------------------------

static void abs_play_track(int idx, double offset_in_track);

void abs_on_eos() {
    if (!ap.active) return;   // local file EOS: no-op (upstream behaviour)
    // Position at end of current track
    if (ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size()) {
        ap.book_position = ap.tracks[ap.track_idx].start_offset + ap.tracks[ap.track_idx].duration;
    }

    int next = ap.track_idx + 1;
    if (next < (int)ap.tracks.size()) {
        g_print("ABS: track finished, chaining to track %d\n", next + 1);
        abs_play_track(next, 0.0);
    } else {
        // Book finished
        g_print("ABS: book finished\n");
        double dur = ap.book_duration > 0 ? ap.book_duration : ap.book_position;
        gAbs.setItemProgress(ap.item_id, dur, dur);
        gAbs.closeSession(ap.session_id);
        ap.session_id.clear();
        ap.track_idx = -1;
        ap.active = false;
        ap.book_position = 0;
        set_player_labels(ap.book_title, "Finished");
        gtk_image_clear(GTK_IMAGE(cover_image));
    }
}

static void abs_load_cover(const std::string& item_id) {
    std::string path = std::string(ABS_COVER_DIR) + "/" + item_id + ".jpg";
    GdkPixbuf* pix = gdk_pixbuf_new_from_file(path.c_str(), NULL);
    if (!pix) {
        mkdir(ABS_COVER_DIR, 0777);
        if (gAbs.getCover(item_id, 400, path)) {
            pix = gdk_pixbuf_new_from_file(path.c_str(), NULL);
        }
    }
    if (pix) {
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(pix, 350, 450, GDK_INTERP_BILINEAR);
        gtk_image_set_from_pixbuf(GTK_IMAGE(cover_image), scaled);
        g_object_unref(scaled);
        g_object_unref(pix);
    }
    // else: leave the previous cover / blank (e-ink friendly)
}

static void abs_play_track(int idx, double offset_in_track) {
    if (idx < 0 || idx >= (int)ap.tracks.size()) return;
    const AbsTrack& tr = ap.tracks[idx];

    char path[256];
    snprintf(path, sizeof(path), "%s/%s_%d.mp3", ABS_CACHE_DIR, ap.item_id.c_str(), tr.index);

    // Download if not cached (LAN: a chapter is a couple of seconds)
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0) {
        g_print("ABS: downloading track %d\n", tr.index);
        mkdir(ABS_CACHE_DIR, 0777);
        if (!gAbs.downloadFile(tr.content_url, path)) {
            remove(path);
            show_abs_error("Failed to download track.\nIs Wi-Fi on and server reachable?");
            return;
        }
    }

    backend.stop();

    ap.track_idx = idx;
    ap.book_position = tr.start_offset + offset_in_track;
    ap.last_sync_position = ap.book_position;
    ap.last_sync_tick = 0;

    g_print("ABS: playing track %d at offset %.0f (book pos %.0f)\n",
            tr.index, offset_in_track, ap.book_position);
    backend.play_file(path, (int)offset_in_track);

    // Track label as chapter indicator
    std::string label = "Chapter " + std::to_string(idx + 1) + "/" +
                        std::to_string(ap.tracks.size());
    set_player_labels(ap.book_title, label);
    abs_load_cover(ap.item_id);
}

void abs_sync_tick() {
    if (!ap.active || !backend.is_playing) return;

    double pos = backend.get_position() / GST_SECOND;
    if (ap.track_idx >= 0 && ap.track_idx < (int)ap.tracks.size())
        ap.book_position = ap.tracks[ap.track_idx].start_offset + pos;

    ap.last_sync_tick++;
    if (ap.last_sync_tick < ABS_SYNC_INTERVAL) return;
    ap.last_sync_tick = 0;

    double delta = ap.book_position - ap.last_sync_position;
    if (delta < 0) delta = 0;   // seeked backwards
    if (ap.session_id.size() &&
        gAbs.syncSession(ap.session_id, ap.book_position, delta,
                        ap.book_duration > 0 ? ap.book_duration : 0)) {
        ap.last_sync_position = ap.book_position;
        g_print("ABS: synced progress %.0fs\n", ap.book_position);
    } else {
        g_print("ABS: sync failed (server unreachable?)\n");
    }
}

void abs_jump_relative(int seconds) {
    if (!ap.active || ap.track_idx < 0) return;
    double pos = backend.get_position() / GST_SECOND;
    double new_track_pos = pos + seconds;
    if (new_track_pos < 0) {
        // Jump into the previous track
        int prev = ap.track_idx - 1;
        if (prev < 0) { abs_play_track(0, 0); return; }
        double remain = -new_track_pos;
        const AbsTrack& pt = ap.tracks[prev];
        abs_play_track(prev, pt.duration - remain > 0 ? pt.duration - remain : 0);
    } else if (ap.track_idx < (int)ap.tracks.size() &&
               new_track_pos > ap.tracks[ap.track_idx].duration) {
        int next = ap.track_idx + 1;
        if (next >= (int)ap.tracks.size()) { abs_on_eos(); return; }
        abs_play_track(next, new_track_pos - ap.tracks[ap.track_idx].duration);
    } else {
        abs_play_track(ap.track_idx, new_track_pos);
    }
}

void abs_shutdown() {
    if (!ap.active) return;
    if (ap.session_id.size()) {
        double delta = ap.book_position - ap.last_sync_position;
        if (delta < 0) delta = 0;
        gAbs.syncSession(ap.session_id, ap.book_position, delta,
                        ap.book_duration > 0 ? ap.book_duration : 0);
        gAbs.closeSession(ap.session_id);
        ap.session_id.clear();
        g_print("ABS: session closed at %.0fs\n", ap.book_position);
    }
    ap.active = false;
}

void abs_replay_current() {
    if (!ap.active || ap.track_idx < 0) return;
    double offset = ap.book_position - ap.tracks[ap.track_idx].start_offset;
    if (offset < 0) offset = 0;
    abs_play_track(ap.track_idx, offset);
}

// Start playback of a library item (from the browser)
static void abs_play_item(const std::string& item_id, const std::string& title) {
    if (ap.active) abs_shutdown();
    backend.stop();

    std::vector<AbsChapter> chapters;
    double cur = 0, udur = 0;
    if (!gAbs.getItemDetail(item_id, chapters, ap.tracks, cur, udur)) {
        show_abs_error("Failed to load book details.\nIs Wi-Fi on and server reachable?");
        return;
    }
    if (ap.tracks.empty()) {
        show_abs_error("This book has no playable tracks.");
        return;
    }

    std::string session_id;
    std::vector<AbsTrack> play_tracks;
    if (!gAbs.startPlayback(item_id, session_id, play_tracks) || play_tracks.empty()) {
        show_abs_error("Failed to start playback session.");
        return;
    }

    ap.active = true;
    ap.session_id = session_id;
    ap.item_id = item_id;
    ap.book_title = title;
    ap.book_duration = udur;
    if (ap.book_duration <= 0) {
        // fall back to last track end
        ap.book_duration = ap.tracks.back().start_offset + ap.tracks.back().duration;
    }

    // Match server session tracks (playMethod 0: contentUrls match the detail list)
    // Detail list is the source of truth for offsets.
    double start_at = cur;
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
            title.c_str(), start_at, idx + 1, (int)ap.tracks.size());
    last_timestamp = 0;
    abs_play_track(idx, offset);
}

// ---------------------------------------------------------------------------------
// Configuration (first run)
// ---------------------------------------------------------------------------------

static bool ensure_config() {
    std::string server = db.getSetting("abs_server");
    std::string token = db.getSetting("abs_token");
    if (!server.empty() && !token.empty()) {
        gAbs.setServer(server, token);
        return true;
    }

    // First run: prompt for server URL
    std::string url = "http://";
    show_dialog_keyboard(url);
    if (url.size() <= 7) return false;

    std::string key;
    show_dialog_keyboard(key);
    if (key.empty()) return false;

    gAbs.setServer(url, key);
    std::string version;
    if (!gAbs.checkServer(version)) {
        show_abs_error("Could not reach Audiobookshelf at\n" + url + "\n(plain HTTP, LAN)");
        return false;
    }
    db.setSetting("abs_server", url);
    db.setSetting("abs_token", key);
    g_print("ABS: configured %s (server %s)\n", url.c_str(), version.c_str());
    return true;
}

// ---------------------------------------------------------------------------------
// Library browser window
// ---------------------------------------------------------------------------------

static GtkWidget* abs_win = NULL;
static GtkWidget* abs_list_box = NULL;
static GtkWidget* abs_status = NULL;
static GtkWidget* abs_page_label = NULL;
static std::vector<AbsItem> abs_page_items;
static std::string abs_library_id;
static int abs_page = 0;

static void abs_clear_list() {
    if (!abs_list_box) return;
    GList* children = gtk_container_get_children(GTK_CONTAINER(abs_list_box));
    for (GList* it = children; it; it = it->next)
        gtk_widget_destroy(GTK_WIDGET(it));
    g_list_free(children);
}

static void on_abs_row_clicked(GtkWidget* widget, gpointer data) {
    (void)widget;
    intptr_t i = (intptr_t)data;
    if (i < 0 || i >= (int)abs_page_items.size()) return;
    AbsItem it = abs_page_items[i];
    if (abs_win) {
        gtk_widget_destroy(abs_win);
        abs_win = NULL;
    }
    abs_play_item(it.id, it.title);
}

static void abs_add_row(const AbsItem& it, size_t index) {
    GtkWidget* row = gtk_event_box_new();

    GtkWidget* hbox = gtk_hbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 6);
    gtk_container_add(GTK_CONTAINER(row), hbox);

    // Cover thumbnail (cached JPEG)
    std::string cover = std::string(ABS_COVER_DIR) + "/" + it.id + ".jpg";
    GdkPixbuf* pix = gdk_pixbuf_new_from_file(cover.c_str(), NULL);
    if (!pix) {
        mkdir(ABS_COVER_DIR, 0777);
        if (gAbs.getCover(it.id, 200, cover))
            pix = gdk_pixbuf_new_from_file(cover.c_str(), NULL);
    }
    if (pix) {
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(pix, is_hires ? 110 : 80,
                                                    is_hires ? 165 : 120,
                                                    GDK_INTERP_BILINEAR);
        gtk_box_pack_start(GTK_BOX(hbox), gtk_image_new_from_pixbuf(scaled), FALSE, FALSE, 0);
        g_object_unref(scaled);
        g_object_unref(pix);
    } else {
        remove(cover.c_str());   // don't cache failed fetches
    }

    GtkWidget* vbox = gtk_vbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    char* m = g_markup_printf_escaped("<span font_desc='Sans Bold %d'>%s</span>",
                                      is_hires ? 18 : 13, it.title.c_str());
    GtkWidget* tl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(tl), m);
    gtk_label_set_line_wrap(GTK_LABEL(tl), TRUE);
    gtk_misc_set_alignment(GTK_MISC(tl), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), tl, FALSE, FALSE, 0);
    g_free(m);

    m = g_markup_printf_escaped("<span font_desc='Sans %d'>%s</span>",
                                is_hires ? 14 : 10, it.author.c_str());
    GtkWidget* al = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(al), m);
    gtk_misc_set_alignment(GTK_MISC(al), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), al, FALSE, FALSE, 0);
    g_free(m);

    std::string prog;
    if (it.finished) {
        prog = "Finished";
    } else if (it.progress > 0) {
        double total = it.duration > 0 ? it.duration : 0;
        double left = total > it.progress ? total - it.progress : 0;
        prog = "In progress";
        if (left > 0) prog += " - " + fmt_time(left) + " left";
    }
    if (!prog.empty()) {
        char* p = g_markup_printf_escaped("<span font_desc='Sans %d' foreground='#555555'>%s</span>",
                                          is_hires ? 13 : 9, prog.c_str());
        GtkWidget* pl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(pl), p);
        gtk_misc_set_alignment(GTK_MISC(pl), 0.0, 0.0);
        gtk_box_pack_start(GTK_BOX(vbox), pl, FALSE, FALSE, 0);
        g_free(p);
    }

    gtk_widget_show_all(row);
    g_signal_connect(row, "button-press-event", G_CALLBACK(on_abs_row_clicked),
                     (gpointer)(intptr_t)index);
    gtk_box_pack_start(GTK_BOX(abs_list_box), row, FALSE, FALSE, 0);
}

static void abs_load_page(int page, const std::string& search) {
    gtk_label_set_text(GTK_LABEL(abs_status), "Loading...");
    abs_clear_list();

    std::vector<AbsItem> items;
    if (!gAbs.getItems(abs_library_id, page, ABS_PAGE_SIZE, search, items)) {
        gtk_label_set_text(GTK_LABEL(abs_status),
                           "Failed to reach server.\nCheck Wi-Fi, server address and API key.");
        return;
    }

    gtk_label_set_text(GTK_LABEL(abs_status), "");
    if (items.empty()) {
        gtk_label_set_text(GTK_LABEL(abs_status),
                           search.empty() ? "Library is empty." : "No results.");
        return;
    }

    abs_page_items = items;
    for (size_t i = 0; i < items.size(); i++) abs_add_row(items[i], i);

    char buf[32];
    snprintf(buf, sizeof(buf), "Page %d", page + 1);
    gtk_label_set_text(GTK_LABEL(abs_page_label), buf);
    abs_page = page;

    // One full screen refresh after the page is built (e-ink friendly)
    if (abs_win) gtk_widget_show_all(abs_win);
}

static void on_abs_prev_clicked(GtkWidget* w, gpointer d) {
    (void)w;(void)d;
    if (abs_page > 0) abs_load_page(abs_page - 1, "");
}

static void on_abs_next_clicked(GtkWidget* w, gpointer d) {
    (void)w;(void)d;
    abs_load_page(abs_page + 1, "");
}

static void on_abs_search_clicked(GtkWidget* w, gpointer d) {
    (void)w;(void)d;
    std::string q;
    show_dialog_keyboard(q);
    if (q.empty()) {
        abs_load_page(0, "");
        return;
    }
    abs_load_page(0, q);
}

static void on_abs_back_clicked(GtkWidget* w, gpointer d) {
    (void)w;(void)d;
    if (abs_win) {
        gtk_widget_destroy(abs_win);
        abs_win = NULL;
    }
}

static bool ensure_library() {
    std::string saved = db.getSetting("abs_library_id");
    if (!saved.empty()) {
        abs_library_id = saved;
        return true;
    }
    std::vector<AbsLibrary> libs;
    if (!gAbs.getLibraries(libs) || libs.empty()) {
        show_abs_error("No libraries found on the server.");
        return false;
    }
    // v1: pick the first book library
    for (size_t i = 0; i < libs.size(); i++) {
        abs_library_id = libs[i].id;
        db.setSetting("abs_library_id", abs_library_id);
        return true;
    }
    return false;
}

void open_abs_browser() {
    if (abs_win) {   // already open
        gtk_window_present(GTK_WINDOW(abs_win));
        return;
    }

    if (!ensure_config()) return;
    std::string version;
    if (!gAbs.checkServer(version)) {
        show_abs_error("Server unreachable.\nCheck Wi-Fi and the server address.");
        return;
    }
    if (!ensure_library()) return;

    abs_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_size_request(abs_win, 600, 800);
    gtk_window_set_title(GTK_WINDOW(abs_win), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    gtk_window_set_modal(GTK_WINDOW(abs_win), TRUE);

    // White background like the main window
    GtkRcStyle* style = gtk_widget_get_modifier_style(abs_win);
    style->bg[GTK_STATE_NORMAL].red = 65535;
    style->bg[GTK_STATE_NORMAL].green = 65535;
    style->bg[GTK_STATE_NORMAL].blue = 65535;
    gtk_widget_modify_style(abs_win, style);

    GtkWidget* vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_add(GTK_CONTAINER(abs_win), vbox);

    // Header
    GtkWidget* header = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(header), 8);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    GtkWidget* back = gtk_button_new_with_label("< Back");
    g_signal_connect(back, "clicked", G_CALLBACK(on_abs_back_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(header), back, FALSE, FALSE, 0);

    GtkWidget* spacer1 = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(header), spacer1, TRUE, TRUE, 0);

    GtkWidget* search = gtk_button_new_with_label("Search");
    g_signal_connect(search, "clicked", G_CALLBACK(on_abs_search_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(header), search, FALSE, FALSE, 0);

    // Status line
    abs_status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(vbox), abs_status, FALSE, FALSE, 0);

    // Book list
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    abs_list_box = gtk_vbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(abs_list_box), 8);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), abs_list_box);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    // Pager
    GtkWidget* pager = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(pager), 8);
    gtk_box_pack_start(GTK_BOX(vbox), pager, FALSE, FALSE, 4);

    GtkWidget* prev = gtk_button_new_with_label("< Prev");
    g_signal_connect(prev, "clicked", G_CALLBACK(on_abs_prev_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(pager), prev, FALSE, FALSE, 0);

    GtkWidget* psp = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(pager), psp, TRUE, TRUE, 0);

    abs_page_label = gtk_label_new("Page 1");
    gtk_box_pack_start(GTK_BOX(pager), abs_page_label, FALSE, FALSE, 0);

    GtkWidget* psp2 = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(pager), psp2, TRUE, TRUE, 0);

    GtkWidget* next = gtk_button_new_with_label("Next >");
    g_signal_connect(next, "clicked", G_CALLBACK(on_abs_next_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(pager), next, FALSE, FALSE, 0);

    gtk_widget_show_all(abs_win);
    abs_load_page(0, "");
}

// ---------------------------------------------------------------------------------
// Debug/testing autostart (env-var gated; used for remote SSH testing)
// ---------------------------------------------------------------------------------

static gboolean abs_debug_autoplay_timer(gpointer data) {
    (void)data;
    if (abs_win && !abs_page_items.empty()) {
        g_print("ABS: debug autoplay of first item\n");
        on_abs_row_clicked(NULL, (gpointer)(intptr_t)0);
    }
    return FALSE;   // one-shot
}

void abs_debug_autostart() {
    if (!getenv("LARK_ABS_TEST")) return;
    g_print("ABS: debug autostart - opening browser\n");
    open_abs_browser();
    if (getenv("LARK_ABS_AUTOPLAY")) {
        g_timeout_add_seconds(4, abs_debug_autoplay_timer, NULL);
    }
}
