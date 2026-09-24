#!/bin/sh
set -e

usage() {
  echo "usage: $0 [debug|debug_asan|release] [args...]" >&2
  exit 1
}

if [ $# -lt 1 ]; then
  usage
fi

MODE="$1"
shift

CC="${CC:-clang}"

# Shared by every mode: the warning set does not vary, so a release build can
# never compile something a debug build rejects.
CFLAGS="-std=c99 -fpie -fno-omit-frame-pointer
-Wall -Wextra -Werror
-Wconversion -Wsign-conversion -Wshadow -Wundef -Wcast-align -Wwrite-strings
-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
-Wold-style-definition -Wredundant-decls -Wpointer-arith -Wvla
-Wswitch-enum -Wformat=2 -Wdouble-promotion -Wimplicit-fallthrough
-Wunused-macros -Wbad-function-cast -Winit-self -Walloca
-Wunreachable-code -Wfloat-equal -Wno-cast-function-type-mismatch"

# `-DNDEBUG` appears in no mode: the asserts are part of the program in all
# three, release included.
case "$MODE" in
debug)
  BIN="a.out.debug"
  MODE_CFLAGS="-g -gsplit-dwarf -O0"
  ;;
debug_asan)
  BIN="a.out.debug_asan"
  MODE_CFLAGS="-g -gsplit-dwarf -O0
-fsanitize=address,undefined -fno-sanitize-recover=all"
  ;;
release)
  # These are the benchmarking flags; a measurement taken with any other set
  # does not count.
  BIN="a.out.release"
  MODE_CFLAGS="-O2 -march=native -flto=full"
  ;;
*)
  echo "unknown mode: $MODE" >&2
  usage
  ;;
esac

set -x

"$CC" $CFLAGS $MODE_CFLAGS -o "$BIN" main.c
time "./$BIN" "$@"
