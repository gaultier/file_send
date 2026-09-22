#!/bin/sh
set -xe

CFLAGS="-fpie -fno-omit-frame-pointer -gsplit-dwarf -march=native -std=c99 -Wall -Wextra -Werror -Wno-cast-function-type-mismatch -g"

# Sanitized build first: UBSan catches the signed overflow/conversion class,
# ASan the stack and heap ones. Note ASan cannot see an overrun *inside* an
# arena -- the whole arena is a single mmap as far as it is concerned.
clang $CFLAGS -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o a.out.san main.c
./a.out.san "$@"

clang $CFLAGS -o a.out main.c
exec ./a.out "$@"
