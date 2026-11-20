#ifndef APP_ACTIONS_H
#define APP_ACTIONS_H

#include <gtk/gtk.h>
#include <string>
#include <functional>

// Glue between the screens (Player / Library / Options) and the app core in
// m4b_player.cpp. Everything here runs on the GTK main thread.

enum { TAB_PLAY = 0, TAB_LIBRARY = 1, TAB_OPTIONS = 2 };
void app_show_tab(int tab);

// --- Player screen ---------------------------------------------------------------

// Cover art shown on the Player screen; NULL shows the generic audiobook icon.
// The pixbuf is scaled to fit and not consumed (caller keeps its reference).
void player_set_cover(GdkPixbuf* pixbuf);
// Book title + author (shown instead of the cover while "title mode" is on).
void player_set_book(const std::string& title, const std::string& author);
// Chapter line, "00:05:07 / 30:00:04" line and "N minutes left" line.
void player_set_info(const std::string& chapter, const std::string& time,
                     const std::string& left);
// Applies the cover/title toggle (persisted as "show_title").
void player_apply_display_mode();
// "25 minutes left in chapter" style text
std::string fmt_left_text(double secs, const char* what);

// --- local (non-Audiobookshelf) books -------------------------------------------

void app_open_local_file(const char* path);   // starts playback and shows Play tab
void app_open_file_dialog();

// On-screen keyboard (a page of the main window, not a separate window). Returns
// immediately; done(ok, text) runs later on the GTK thread. ok is false when
// the user cancelled. password hides the text behind bullets (with a Show key).
void app_keyboard(const std::string& caption, const std::string& initial, bool password,
                  std::function<void(bool ok, const std::string& text)> done);
// Used by the keyboard page to take over / give back the main window.
void app_set_keyboard_visible(bool visible);

// --- Options actions -------------------------------------------------------------

struct SleepOption {
    const char* label;
    int minutes;       // 0 = off, -1 = end of chapter, >0 = minutes
};
int app_sleep_option_count();
const SleepOption& app_sleep_option(int index);
int app_sleep_mode();                 // index into the sleep options
void app_set_sleep_mode(int index);

bool app_refresh_disabled();
void app_set_refresh_disabled(bool disabled);
bool app_backlight_disabled();
void app_set_backlight_disabled(bool disabled);
bool app_auto_suspend();
void app_set_auto_suspend(bool enabled);

void app_open_bluetooth_settings();
void app_exit();

// Soft/full e-ink refresh shortly after the GTK repaint has been flushed.
void app_schedule_refresh(bool full);

#endif // APP_ACTIONS_H
