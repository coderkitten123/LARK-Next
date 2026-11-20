# LARKPlayer host preview harness

## Requirements

On Debian 13 (and compatible Debian-based distributions), install the
build and preview dependencies:

```bash
sudo apt install build-essential pkg-config libgtk2.0-dev libxml2-dev \
    libx11-dev libxtst-dev xserver-xephyr x11-utils x11-xserver-utils \
    curl python3
```

Builds the real UI sources (m4b_player, abs_client, abs_ui, eink_refresh, cJSON, plus ui_kit.cpp /
options_ui.cpp when present) on the Linux host against fakes, and renders them in Xephyr `:7`
(1072x1448) so UI changes can be screenshotted. Everything is written to `/tmp/lark-next-preview/`.

## Files
- `stubs/gst/gst.h`, `stubs/sqlite3.h` - header stubs (no gstreamer/sqlite needed)
- `fake_backend.cpp` - MusicBackend/Decoder with a simulated clock (same is_playing/is_paused semantics
  as the real one; 6x10min fake chapters, 3600 s duration, "Fake Author"; EOS/error callbacks never fire;
  streams (http URLs) report duration 0 like the real backend)
- `fake_db.cpp` - in-memory DatabaseManager; settings persisted to `$LARK_PREVIEW_SETTINGS`
  (default `/tmp/lark-next-preview/settings.txt`), 3 preloaded history books, bookmarks in memory
- `fake_lipc.cpp` - LIPC fake; prints `LIPC set <svc> <prop>=<val>` / `LIPC get ...` to stdout (-> app.log)
- `fake_abs_server.py [port=8099] [token_ttl=3600]` - fake Audiobookshelf (key `key123`, login `liam`/`secret`,
  `/login` tokens expire after TTL -> 401; env `FAKE_ABS_TOKEN_TTL`, `FAKE_ABS_SORT=1` to honour title sort).
  14 books, item `li_001` = "Harry Potter and the Chamber Of Secrets (full-cast edition)", 19 chapters, 46805 s, 25%.
- `pathshim.c` - LD_PRELOAD redirecting `/mnt/us/...` to `/tmp/lark-next-preview/mnt_us/...` (covers/cache/db paths)
- `xswipe.c` - drag injector; `xtap` comes from `tools/xtap.c`

## Commands

```bash
    ./build.sh                       # SRC_DIR=<tree> to build another tree (default: repo root)
    ./run.sh [fresh|configured] [file.m4b]   # starts Xephyr :7, fake server, app (log: app.log, server.log)
    ./shot.sh NAME                   # -> /tmp/lark-next-preview/NAME.png
    ./tap.sh X Y [count]             # left click
    ./swipe.sh X1 Y1 X2 Y2 [steps] [step_ms]   # press/drag/release (kinetic scroll tests)
```

`configured` seeds abs_server=http://127.0.0.1:8099, abs_token=key123, abs_enabled=1, abs_user=, abs_pass=secret.
`fresh` deletes the settings file. App env: `LARK_WINDOW_SIZE=1072x1448` is set (used only if the app honours it).

## Caveats
- No window manager: windows sit at 0,0 without decorations; transient dialogs are centred on their parent by GTK.
- `GTK2_RC_FILES=/dev/null` is set so the host GTK theme does not leak in (otherwise buttons render dark).
- `eink_refresh: ioctl failed` lines in app.log are expected (no framebuffer).
- Root window colour is grey (#707070); set after the app connects because Xephyr resets it when no client is attached.
- `system()/popen()` children (wget etc.) are not covered by the path shim.
