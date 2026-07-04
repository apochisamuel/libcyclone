#!/bin/bash -eu

cd "$SRC"

# Compile all C sources
for f in src/*.c; do
    $CC $CFLAGS -Isrc -c "$f" -o "$(basename "$f" .c).o"
done

# Create static library
ar rcs libcyclone.a *.o

# Compile and link each fuzzer
for fuzzer_src in fuzz/*_fuzzer.cc; do
    fuzzer_name=$(basename "$fuzzer_src" .cc)
    $CXX $CXXFLAGS -Isrc "$fuzzer_src" libcyclone.a \
        $LIB_FUZZING_ENGINE -o "$OUT/$fuzzer_name"
done

# Copy seed corpora
for corpus_dir in fuzz/corpus/*/; do
    fuzzer_name=$(basename "$corpus_dir")
    if [ -d "$corpus_dir" ]; then
        mkdir -p "$OUT/${fuzzer_name}_corpus"
        cp "$corpus_dir"* "$OUT/${fuzzer_name}_corpus/" 2>/dev/null || true
        cd "$OUT"
        zip -j "${fuzzer_name}_seed_corpus.zip" "${fuzzer_name}_corpus/"* 2>/dev/null || true
        rm -rf "${fuzzer_name}_corpus"
        cd "$SRC"
    fi
done
