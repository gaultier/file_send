#!/bin/sh
set -xe

clang -fpie -fno-omit-frame-pointer -gsplit-dwarf -march=native -std=c99 -Wall -Wextra -Werror -Wno-cast-function-type-mismatch -g main.c
exec ./a.out "$@"
