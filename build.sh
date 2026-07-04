#!/bin/bash
set -e

CC="${CC:-gcc}"
CXX="${CXX:-g++}"
CFLAGS="${CFLAGS:--Wall -Wextra -Wpedantic -std=c11 -O2}"
CXXFLAGS="${CXXFLAGS:--Wall -Wextra -std=c++17 -O2}"

mkdir -p build

echo "Compiling sources..."
for f in src/*.c; do
    name=$(basename "$f" .c)
    echo "  $name.c"
    $CC $CFLAGS -Isrc -c "$f" -o "build/${name}.o"
done

echo "Creating libcyclone.a..."
ar rcs build/libcyclone.a build/*.o

echo "Done. Library at build/libcyclone.a"
