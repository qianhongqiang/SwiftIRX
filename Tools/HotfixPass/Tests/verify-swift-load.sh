#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture.swift"
TOOLCHAIN="$ROOT/Tools/HotfixPass/.toolchain/swift-llvm-project"
SWIFT_LLVM_COMMIT="8f0d2ca924db37c8f8161d55c21b9097b05b72f3"
OUTPUT_BASE="$(mktemp "${TMPDIR:-/tmp}/hotfix-pass.XXXXXX")"
OUTPUT="$OUTPUT_BASE.ll.tmp"

cleanup() {
  rm -f "$OUTPUT_BASE" "$OUTPUT"
}
trap cleanup EXIT

SWIFT_VERSION="$(xcrun swiftc --version 2>&1)"
if ! grep -Fq "Apple Swift version 6.2.4" <<<"$SWIFT_VERSION"; then
  echo "error: expected Apple Swift version 6.2.4" >&2
  echo "$SWIFT_VERSION" >&2
  exit 1
fi

LLVM_CONFIG="/opt/homebrew/opt/llvm@19/bin/llvm-config"
LLVM_VERSION="$($LLVM_CONFIG --version)"
if [[ "$LLVM_VERSION" != "19.1.7" ]]; then
  echo "error: expected Homebrew LLVM 19.1.7, found $LLVM_VERSION" >&2
  exit 1
fi

PINNED_REVISION="$(git -C "$TOOLCHAIN" rev-parse HEAD 2>/dev/null || true)"
if [[ "$PINNED_REVISION" != "$SWIFT_LLVM_COMMIT" ]]; then
  echo "error: expected Swift LLVM commit $SWIFT_LLVM_COMMIT" >&2
  echo "error: configure the plugin to bootstrap its pinned headers" >&2
  exit 1
fi

VERSION_FILE="$TOOLCHAIN/cmake/Modules/LLVMVersion.cmake"
for version_part in \
  "LLVM_VERSION_MAJOR 19" \
  "LLVM_VERSION_MINOR 1" \
  "LLVM_VERSION_PATCH 5"; do
  if ! grep -Fq "$version_part" "$VERSION_FILE"; then
    echo "error: pinned Swift LLVM headers are not version 19.1.5" >&2
    exit 1
  fi
done

if [[ ! -f "$PLUGIN" ]]; then
  echo "error: pass plugin not found at $PLUGIN" >&2
  exit 1
fi

xcrun swiftc \
  -O \
  -emit-ir \
  -parse-as-library \
  -load-pass-plugin="$PLUGIN" \
  "$FIXTURE" \
  -o "$OUTPUT"

grep -Fq "hotfix-pass-loaded" "$OUTPUT"
