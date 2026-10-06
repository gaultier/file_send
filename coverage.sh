#!/bin/sh
set -e

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# `-DWITH_TESTS`, or the binary has no `test` command to run and the whole
# report comes back as the one `main` path.
clang -std=c99 -g -DWITH_TESTS -fprofile-instr-generate -fcoverage-mapping \
  -o "$OUT/cov" main.c
# Once with no filter, once with one, so the filter path is exercised too.
LLVM_PROFILE_FILE="$OUT/cov-%p.profraw" "$OUT/cov" test >/dev/null 2>&1
LLVM_PROFILE_FILE="$OUT/cov-%p.profraw" "$OUT/cov" test bytes >/dev/null 2>&1
# Every `.c` in the tree, not a hand-kept list: the platform files that this
# build did not compile carry no coverage data and llvm-cov skips them, whereas
# a list silently stops reporting on a file the day one is added.
SOURCES=$(echo *.c)

xcrun llvm-profdata merge -sparse "$OUT"/cov-*.profraw -o "$OUT/cov.profdata"

xcrun llvm-cov report "$OUT/cov" -instr-profile="$OUT/cov.profdata" $SOURCES

# Uncovered lines, if any.
echo
echo "Uncovered lines:"
xcrun llvm-cov show "$OUT/cov" -instr-profile="$OUT/cov.profdata" $SOURCES \
  | awk '{ if (match($0, /^ *[0-9]+\| *0\|/)) print }' | sed 's/|.*0|/|/'
