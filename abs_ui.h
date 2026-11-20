#ifndef ABS_UI_H
#define ABS_UI_H

#include <gtk/gtk.h>
#include <string>

// Stage 2 UI + Stage 3 playback glue for Audiobookshelf.
// All network work happens on a background worker thread; UI callbacks are
// marshalled back with g_idle_add, so the GTK main loop never blocks.

// Library screen (the middle tab). lib_page_new() builds it once; the app calls
// lib_page_activate() every time the tab is shown: it connects to Audiobookshelf
// on first use, or lists local books when Audiobookshelf is switched off.
GtkWidget* lib_page_new();
void lib_page_activate();

// Audiobookshelf settings (edited on the Options screen).
bool abs_settings_enabled();
// Server/login details changed: the library reconnects next time it is shown.
void abs_settings_changed();
// Tests the saved server + login/API key on the worker thread; done() runs on
// the GTK thread with a short human-readable result.
typedef void (*AbsTestDone)(bool ok, const std::string& message, gpointer user);
void abs_test_connection(AbsTestDone done, gpointer user);

// Called from the 1-second update_ui() tick. Updates book_position and
// enqueues a throttled server progress sync (never blocks).
void abs_sync_tick();

// Flushes the current position to the server immediately, bypassing the
// periodic throttle. Call on pause, on leaving the Player tab, and when the
// app loses focus, so another Audiobookshelf client (or this book reopened
// later) never sees more than a few seconds of stale progress. No-op when no
// ABS book is active.
void abs_force_sync();

// Finalizes an ABS session left open by an earlier crash/kill (normal clean
// shutdowns clear the record themselves). Call once at startup.
void abs_reconcile_stale_session();

// EOS hook: registered on the backend. Advances ABS tracks / finishes the book.
void abs_on_eos();

// Backend callback adapter (matches EosCallback signature)
void abs_on_eos_wrapper(void* user_data);

// True while an ABS (server) book is playing instead of a local file.
bool abs_is_active();

// Final progress sync + session close, done on the worker thread. Called on
// app close (non-ABS paths) and when another book is opened over ABS playback.
void abs_shutdown();

// App-exit final progress sync + session close, responsive-close friendly:
//   abs_begin_shutdown_sync() snapshots the session on the UI thread and runs
//   the final sync + close on a background thread (close responds instantly,
//   even when the server is unreachable).
//   abs_wait_shutdown_sync(ms) blocks at most ms for that sync to finish.
void abs_begin_shutdown_sync();
void abs_wait_shutdown_sync(int timeout_ms);

// Jump relative (seconds) within the current ABS book.
void abs_jump_relative(int seconds);

// Replays the current ABS track at the last known book position
// (used when the user presses Play after a stop).
void abs_replay_current();

// Chapter list dialog for the current ABS book (server chapter metadata).
// Selecting a chapter starts playback at that chapter.
void abs_show_chapter_list();

// Debug/testing helpers (enabled via env vars, no-ops otherwise):
//   LARK_ABS_TEST=1      -> app auto-opens the ABS browser after start
//   LARK_ABS_AUTOPLAY=1  -> browser auto-plays the first item in the page
void abs_debug_autostart();

// Updates the chapter / time / "left in chapter" lines on the Player screen.
// Called from update_ui() every second while an ABS book is active.
void abs_time_display(bool dispUpdate);

// Seconds until the next chapter ends at the current book position, or -1
// when unknown (used by the sleep timer's "stop at chapter end").
double abs_seconds_to_chapter_end();

#endif // ABS_UI_H
