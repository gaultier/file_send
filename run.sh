#!/bin/sh
set -xe

CFLAGS="-fpie -fno-omit-frame-pointer -gsplit-dwarf -march=native -std=c99 -Wall -Wextra -Werror -Wno-cast-function-type-mismatch -g"

clang $CFLAGS -O0 -fsanitize=address,undefined -fno-sanitize-recover=all -o a.out.san main.c
time ./a.out.san "$@"

clang $CFLAGS -o a.out main.c
time ./a.out "$@"
