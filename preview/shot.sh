#!/bin/bash
# ./shot.sh NAME  -> /tmp/lark-next-preview/NAME.png
OUT="${OUT_DIR:-${TMPDIR:-/tmp}/lark-next-preview}"
mkdir -p "$OUT"
NAME="${1:-shot}"
DISPLAY=:7 import -window root "$OUT/$NAME.png" && echo "$OUT/$NAME.png"
