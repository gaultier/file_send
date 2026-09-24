#!/bin/sh
set -xe

CFLAGS="-fpie -fno-omit-frame-pointer -gsplit-dwarf -march=native -std=c99 -Wall -Wextra -Wsign-conversion -Werror -Wno-cast-function-type-mismatch -g 
-Wconversion -Wshadow -Wundef -Wcast-align -Wwrite-strings
-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
-Wold-style-definition -Wredundant-decls -Wpointer-arith -Wvla
-Wswitch-enum -Wformat=2 -Wdouble-promotion -Wimplicit-fallthrough
-Wunused-macros -Wbad-function-cast -Winit-self -Walloca
-Wunreachable-code -Wfloat-equal"

clang $CFLAGS -O0 -fsanitize=address,undefined -fno-sanitize-recover=all -o a.out.san main.c
time ./a.out.san "$@"

clang $CFLAGS -o a.out main.c
time ./a.out "$@"
