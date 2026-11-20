#!/bin/bash
# Build the host preview binary.  SRC_DIR=<tree> ./build.sh   (default: the live repo)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="${SRC_DIR:-$(cd "$HERE/.." && pwd)}"
OUT_DIR="${OUT_DIR:-${TMPDIR:-/tmp}/lark-next-preview}"
OBJ="$OUT_DIR/obj"
mkdir -p "$OBJ"

GTK_FLAGS="$(pkg-config --cflags gtk+-2.0 libxml-2.0)"
GTK_LIBS="$(pkg-config --libs gtk+-2.0 libxml-2.0)"
CXXFLAGS="-std=c++11 -g -O0 -Wall -Wextra -Wno-deprecated-declarations -pthread -I$HERE/stubs -I$SRC_DIR $GTK_FLAGS"

SRCS=(m4b_player.cpp abs_client.cpp abs_ui.cpp eink_refresh.cpp ram_cache.cpp)
for opt in ui_kit.cpp options_ui.cpp keyboard_ui.cpp; do
    [ -f "$SRC_DIR/$opt" ] && SRCS+=("$opt")
done

OBJS=()
pids=()
compile() {  # compile <src> <obj>
    g++ $CXXFLAGS -c "$1" -o "$2"
}
for s in "${SRCS[@]}"; do
    o="$OBJ/$(basename "${s%.cpp}").o"; OBJS+=("$o")
    compile "$SRC_DIR/$s" "$o" & pids+=($!)
done
for s in fake_backend.cpp fake_db.cpp fake_lipc.cpp; do
    o="$OBJ/$(basename "${s%.cpp}").o"; OBJS+=("$o")
    compile "$HERE/$s" "$o" & pids+=($!)
done
# cJSON is C
gcc -g -O0 -Wall -I"$SRC_DIR" -c "$SRC_DIR/third_party/cJSON/cJSON.c" -o "$OBJ/cJSON.o" & pids+=($!)
OBJS+=("$OBJ/cJSON.o")

fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ $fail -eq 0 ] || { echo "BUILD FAILED" >&2; exit 1; }

g++ -pthread -rdynamic "${OBJS[@]}" -o "$OUT_DIR/larkplayer_preview" \
    $GTK_LIBS -lgthread-2.0 -ldl -lm -lrt
echo "built $OUT_DIR/larkplayer_preview (SRC_DIR=$SRC_DIR)"

# input injection helper
if [ ! -x "$OUT_DIR/xtap" ] || [ "$SRC_DIR/tools/xtap.c" -nt "$OUT_DIR/xtap" ] 2>/dev/null; then
    if [ -f "$SRC_DIR/tools/xtap.c" ]; then
        gcc -O1 "$SRC_DIR/tools/xtap.c" -o "$OUT_DIR/xtap" -lX11 -lXtst 2>/dev/null || echo "warning: xtap build failed" >&2
    fi
fi
gcc -O1 "$HERE/xswipe.c" -o "$OUT_DIR/xswipe" -lX11 -lXtst 2>/dev/null || echo "warning: xswipe build failed" >&2
gcc -shared -fPIC -O1 "$HERE/pathshim.c" -o "$OUT_DIR/pathshim.so" -ldl || echo "warning: pathshim build failed" >&2
