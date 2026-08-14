#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PASS_ROOT="$ROOT/Tools/HotfixPass"
PINNED_SOURCE="$PASS_ROOT/.toolchain/swift-llvm-project"
PINNED_COMMIT="8f0d2ca924db37c8f8161d55c21b9097b05b72f3"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-bootstrap.XXXXXX")"

cleanup() {
  rm -rf "$TEMP"
}
trap cleanup EXIT

cmake -P "$PASS_ROOT/Tests/cmake/validate-version-parsers.cmake"

if [[ "$(git -C "$PINNED_SOURCE" rev-parse HEAD)" != "$PINNED_COMMIT" ]]; then
  echo "error: configure HotfixPass before running bootstrap guard tests" >&2
  exit 1
fi

INCOMPLETE_CACHE="$TEMP/incomplete-cache"
mkdir -p "$INCOMPLETE_CACHE/swift-llvm-project/.git"
cmake \
  -S "$PASS_ROOT" \
  -B "$TEMP/incomplete-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSWIFT_LLVM_CACHE_DIR="$INCOMPLETE_CACHE" \
  -DSWIFT_LLVM_REPOSITORY="file://$PINNED_SOURCE" \
  >/dev/null

if [[ "$(git -C "$INCOMPLETE_CACHE/swift-llvm-project" rev-parse HEAD)" != \
      "$PINNED_COMMIT" ]]; then
  echo "error: incomplete cache was not rebuilt at the pinned commit" >&2
  exit 1
fi

touch "$INCOMPLETE_CACHE/swift-llvm-project/untracked-header"
if cmake \
    -S "$PASS_ROOT" \
    -B "$TEMP/dirty-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSWIFT_LLVM_CACHE_DIR="$INCOMPLETE_CACHE" \
    -DSWIFT_LLVM_REPOSITORY="file://$PINNED_SOURCE" \
    >"$TEMP/dirty.log" 2>&1; then
  echo "error: dirty Swift LLVM checkout was accepted" >&2
  exit 1
fi
grep -Fq "Swift LLVM cache must be pristine" "$TEMP/dirty.log"
