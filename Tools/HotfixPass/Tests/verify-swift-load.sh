#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture.swift"
LLVM_ROOT="/opt/homebrew/opt/llvm@19/bin"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-swift-bitcode.XXXXXX")"

cleanup() {
  rm -rf "$TEMP"
}
trap cleanup EXIT

SWIFT_VERSION="$(xcrun swiftc --version 2>/dev/null)"
SWIFT_IDENTITY="$(
  printf '%s\n' "$SWIFT_VERSION" | sed -n '/^Apple Swift version /p'
)"
EXPECTED_SWIFT_IDENTITY="Apple Swift version 6.2.4 (swiftlang-6.2.4.1.4 clang-1700.6.4.2)"
if [[ "$SWIFT_IDENTITY" != "$EXPECTED_SWIFT_IDENTITY" ]]; then
  echo "error: expected $EXPECTED_SWIFT_IDENTITY" >&2
  echo "$SWIFT_VERSION" >&2
  exit 1
fi

LLVM_VERSION="$($LLVM_ROOT/llvm-config --version)"
if [[ "$LLVM_VERSION" != "19.1.7" ]]; then
  echo "error: expected Homebrew LLVM 19.1.7, found $LLVM_VERSION" >&2
  exit 1
fi

if [[ ! -f "$PLUGIN" ]]; then
  echo "error: pass plugin not found at $PLUGIN" >&2
  exit 1
fi

xcrun swiftc \
  -O \
  -emit-bc \
  -parse-as-library \
  "$FIXTURE" \
  -o "$TEMP/input.bc"

"$LLVM_ROOT/opt" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  "$TEMP/input.bc" \
  -o "$TEMP/transformed.bc"

"$LLVM_ROOT/llvm-dis" \
  "$TEMP/transformed.bc" \
  -o "$TEMP/transformed.ll"

grep -Fq "hotfix-pass-loaded" "$TEMP/transformed.ll"
grep -Fq "ir_hotfix_invoke" "$TEMP/transformed.ll"
grep -Fq ".hotfix_original" "$TEMP/transformed.ll"
