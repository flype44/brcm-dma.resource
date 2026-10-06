#!/bin/sh
# Host test of the TagList code. Run from the root of the repository.
set -e
mkdir -p build-host
gcc -Wall -Wno-pointer-sign -Wno-int-to-pointer-cast -Itest/host/stubs -Isrc -Iinclude -include test/host/prelude.h \
    test/host/tags_test.c src/tags.c -o build-host/tags_test
./build-host/tags_test
