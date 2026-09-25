#!/bin/sh
set -ex

# Build the unity build for every {arch} x {os} pair, with the warning set
# `run.sh` uses. Compiles and then links, because a missing library only shows
# up at the link step.

WARN="-Wall -Wextra -Werror
-Wconversion -Wsign-conversion -Wshadow -Wundef -Wcast-align -Wwrite-strings
-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
-Wold-style-definition -Wredundant-decls -Wpointer-arith -Wvla
-Wswitch-enum -Wformat=2 -Wdouble-promotion -Wimplicit-fallthrough
-Wunused-macros -Wbad-function-cast -Winit-self -Walloca
-Wunreachable-code -Wfloat-equal -Wno-cast-function-type-mismatch"

OUT="${1:-.}"
rc=0

for arch in x86_64 aarch64; do
  for os in windows linux macos; do
    target="$arch-$os"
    # The same feature-test macros `run.sh` passes; Windows needs neither.
    case "$os" in
    linux)   defs="-D_POSIX_C_SOURCE=200809L" ;;
    macos)   defs="-D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE" ;;
    windows) defs="" ;;
    esac

    status=""
    for stage in compile link; do
      case "$stage" in
      compile) args="-c -o $OUT/$target.o" ;;
      link)    args="-o $OUT/$target.bin" ;;
      esac

      log="$OUT/$target.$stage.log"
      if zig cc -std=c99 $defs $WARN --target="$target" $args main.c \
          >"$log" 2>&1; then
        status="$status $stage=ok"
      else
        status="$status $stage=FAIL"
        rc=1
      fi
    done

    printf '%-18s%s\n' "$target" "$status"
  done
done

exit $rc
