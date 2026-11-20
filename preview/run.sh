#!/bin/bash
# ./run.sh [fresh|configured]   - (re)start Xephyr :7, fake ABS server and the preview app
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT_DIR:-${TMPDIR:-/tmp}/lark-next-preview}"
mkdir -p "$OUT"
export LARK_PREVIEW_SETTINGS="${LARK_PREVIEW_SETTINGS:-$OUT/settings.txt}"
MODE="$1"

if ! pgrep -f "^Xephyr :7 " >/dev/null; then
    nohup Xephyr :7 -softCursor -screen 1072x1448 -dpi 96 -ac -br -noreset >"$OUT/xephyr.log" 2>&1 &
    for i in $(seq 50); do DISPLAY=:7 xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
fi

pkill -f "^$OUT/larkplayer_preview" 2>/dev/null
pkill -f "^python3 .*fake_abs_server[.]py" 2>/dev/null
sleep 0.3

case "$MODE" in
    fresh) rm -f "$LARK_PREVIEW_SETTINGS" ;;
    configured)
        cat > "$LARK_PREVIEW_SETTINGS" <<EOS
abs_server=http://127.0.0.1:8099
abs_token=key123
abs_enabled=1
abs_user=liam
abs_pass=secret
EOS
        ;;
esac

nohup python3 "$HERE/fake_abs_server.py" ${FAKE_ABS_PORT:-8099} >"$OUT/server.log" 2>&1 &
for i in $(seq 30); do curl -s -o /dev/null http://127.0.0.1:${FAKE_ABS_PORT:-8099}/status && break; sleep 0.1; done

[ -x "$OUT/larkplayer_preview" ] || { echo "no binary; run build.sh first" >&2; exit 1; }
mkdir -p "$OUT/mnt_us"
DISPLAY=:7 GTK2_RC_FILES=/dev/null LD_PRELOAD="$OUT/pathshim.so" LARK_PREVIEW_MNT_US="$OUT/mnt_us" LARK_WINDOW_SIZE=1072x1448 LARK_PREVIEW_SETTINGS="$LARK_PREVIEW_SETTINGS" \
    nohup "$OUT/larkplayer_preview" "${@:2}" >"$OUT/app.log" 2>&1 &
echo "started app pid $! (log $OUT/app.log, server log $OUT/server.log, settings $LARK_PREVIEW_SETTINGS)"
sleep 1
# set root colour only while a client (the app) is connected: Xephyr resets to black when the last client leaves
xsetroot -display :7 -solid "#707070" 2>/dev/null
sleep 0.5
