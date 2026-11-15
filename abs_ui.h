#ifndef ABS_UI_H
#define ABS_UI_H

// Stage 2 UI + Stage 3-lite playback glue for Audiobookshelf.
// Hooks called from m4b_player.cpp:

// Opens the library browser window (first-run: asks for server URL + API key).
void open_abs_browser();

// Called from the 1-second update_ui() tick. Throttled progress sync.
void abs_sync_tick();

// EOS hook: registered on the backend. Advances ABS tracks / finishes the book.
void abs_on_eos();

// Backend callback adapter (matches EosCallback signature)
void abs_on_eos_wrapper(void* user_data);

// True while an ABS (server) book is playing instead of a local file.
bool abs_is_active();

// Final progress sync + session close. Called on app close and when a local
// file is opened over ABS playback.
void abs_shutdown();

// Jump relative (seconds) within the current ABS book.
void abs_jump_relative(int seconds);

// Replays the current ABS track at the last known book position
// (used when the user presses Play after a stop).
void abs_replay_current();

// Debug/testing helpers (enabled via env vars, no-ops otherwise):
//   LARK_ABS_TEST=1      -> app auto-opens the ABS browser after start
//   LARK_ABS_AUTOPLAY=1  -> browser auto-plays the first item in the page
void abs_debug_autostart();

#endif // ABS_UI_H
