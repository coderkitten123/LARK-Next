#!/bin/bash
# ./tap.sh X Y [count]  - left click at X,Y on :7
OUT="${OUT_DIR:-${TMPDIR:-/tmp}/lark-next-preview}"
[ -x "$OUT/xtap" ] || { echo "missing $OUT/xtap (run build.sh)" >&2; exit 1; }
DISPLAY=:7 "$OUT/xtap" "$@"
