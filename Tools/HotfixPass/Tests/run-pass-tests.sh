#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/scalars.ll"
OPT="/opt/homebrew/opt/llvm@19/bin/opt"
FILECHECK="/opt/homebrew/opt/llvm@19/bin/FileCheck"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-pass-scalars.XXXXXX")"

cleanup() {
  rm -rf "$TEMP"
}
trap cleanup EXIT

if [[ ! -f "$PLUGIN" ]]; then
  echo "error: pass plugin not found at $PLUGIN" >&2
  exit 1
fi

"$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/named.ll"
"$FILECHECK" \
  --check-prefixes=CHECK,NO-CLONES \
  "$FIXTURE" \
  <"$TEMP/named.ll"

"$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes='hotfix-instrument,hotfix-instrument' \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/repeated.ll"
"$FILECHECK" \
  --check-prefixes=CHECK,NO-CLONES \
  "$FIXTURE" \
  <"$TEMP/repeated.ll"

if [[ "$(grep -Fc 'define private swiftcc i64 @integerTarget.hotfix_original' "$TEMP/repeated.ll")" != "1" ]]; then
  echo "error: repeated pass instrumentation was not idempotent" >&2
  exit 1
fi
if ! grep -Fq '@llvm.compiler.used = appending global [16 x ptr]' "$TEMP/repeated.ll"; then
  echo "error: repeated pass duplicated retained function entries" >&2
  exit 1
fi

"$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes='default<O0>' \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/automatic.ll"
"$FILECHECK" \
  --check-prefix=AUTO \
  "$FIXTURE" \
  <"$TEMP/automatic.ll"

"$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes='default<O2>' \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/optimized.ll"
"$FILECHECK" \
  --check-prefix=AUTO \
  "$FIXTURE" \
  <"$TEMP/optimized.ll"
"$FILECHECK" \
  --check-prefix=OPT \
  "$FIXTURE" \
  <"$TEMP/optimized.ll"
"$FILECHECK" \
  --check-prefix=RETAIN \
  "$FIXTURE" \
  <"$TEMP/optimized.ll"
