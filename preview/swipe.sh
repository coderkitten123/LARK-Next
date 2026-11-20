#!/bin/bash
# ./swipe.sh X1 Y1 X2 Y2 [steps] [step_ms]  - press, drag in steps, release on :7
OUT="${OUT_DIR:-${TMPDIR:-/tmp}/lark-next-preview}"
[ -x "$OUT/xswipe" ] || { echo "missing $OUT/xswipe (run build.sh)" >&2; exit 1; }
DISPLAY=:7 "$OUT/xswipe" "$@"
