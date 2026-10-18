#!/usr/bin/env bash
set -euo pipefail

SUBMISSION_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if (( $# > 1 )) || { (( $# == 1 )) && [[ "$1" != --rebuild ]]; }; then
    echo 'Usage: bash build.sh [--rebuild]' >&2
    exit 1
fi
# Ingestion may invoke build.sh even when a working binary is supplied.
if (( $# == 0 )) && [[ -f "$SUBMISSION_ROOT/router" ]]; then
    chmod 755 "$SUBMISSION_ROOT/router"
    if bash "$SUBMISSION_ROOT/run.sh" --help >/dev/null; then
        echo 'Using the supplied router. Pass --rebuild to compile from source.'
        exit 0
    fi
fi
SUBMISSION_CXX="${CXX:-g++}"
command -v "$SUBMISSION_CXX" >/dev/null
mapfile -d '' SUBMISSION_SOURCES < <(find "$SUBMISSION_ROOT/src" -type f -name '*.cpp' -print0 | sort -z)
if (( ${#SUBMISSION_SOURCES[@]} == 0 )); then
    echo 'No C++ sources found in the submission.' >&2
    exit 1
fi
# Compile to a temporary file so a failed rebuild preserves the shipped binary.
SUBMISSION_BINARY="$(mktemp "$SUBMISSION_ROOT/.router-build.XXXXXX")"
trap 'rm -f -- "$SUBMISSION_BINARY"' EXIT
"$SUBMISSION_CXX" -std=c++20 -O3 -DNDEBUG -Wall -Wextra -pedantic \
    -I"$SUBMISSION_ROOT/src" "${SUBMISSION_SOURCES[@]}" -ldl -o "$SUBMISSION_BINARY"
chmod 755 "$SUBMISSION_BINARY"
mv -f -- "$SUBMISSION_BINARY" "$SUBMISSION_ROOT/router"
echo "Built $SUBMISSION_ROOT/router"
