#!/bin/sh
# Run from any directory. A fresh object tree includes unexecuted application
# files (.gcno) and cannot inherit hits (.gcda) from a previous test run.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
objects=$(mktemp -d "$PWD/build/coverage-XXXXXX")
trap 'rm -rf "$objects"' EXIT

cmake -S . -B "$objects" -DCMAKE_BUILD_TYPE=Debug -DBM_BUILD_TESTS=ON -DBM_ENABLE_COVERAGE=ON
cmake --build "$objects" --config Debug --parallel 4
ctest --test-dir "$objects" -C Debug --output-on-failure --no-tests=error
gcovr --gcov-executable "${GCOV:-gcov}" --root . --filter 'src/' --fail-under-line 93 \
    --txt-summary --html-details build/coverage.html --html-single-page "$objects"
