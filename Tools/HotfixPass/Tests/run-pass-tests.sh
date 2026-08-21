#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/scalars.ll"
SWIFT_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture.swift"
OPT="/opt/homebrew/opt/llvm@19/bin/opt"
FILECHECK="/opt/homebrew/opt/llvm@19/bin/FileCheck"
LLVM_DIS="/opt/homebrew/opt/llvm@19/bin/llvm-dis"
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
  -o "$TEMP/named.ll" \
  2>"$TEMP/named.stderr"
EXPECTED_DIAGNOSTICS=$'[HotfixPass] skip unsupported: unsupported return type\n[HotfixPass] skip unsupportedPointer: unsupported non-receiver pointer argument\n[HotfixPass] skip variadicTarget: variadic function'
if [[ "$(<"$TEMP/named.stderr")" != "$EXPECTED_DIAGNOSTICS" ]]; then
  echo "error: named pass diagnostics differ from the expected deterministic output" >&2
  diff -u <(printf '%s\n' "$EXPECTED_DIAGNOSTICS") "$TEMP/named.stderr" >&2 || true
  exit 1
fi
"$FILECHECK" \
  --check-prefixes=CHECK,NO-CLONES \
  "$FIXTURE" \
  <"$TEMP/named.ll"

"$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes='hotfix-instrument,hotfix-instrument' \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/repeated.ll" \
  2>"$TEMP/repeated.stderr"
if ! cmp -s "$TEMP/named.stderr" "$TEMP/repeated.stderr"; then
  echo "error: repeated pass duplicated or changed skip diagnostics" >&2
  exit 1
fi
"$FILECHECK" \
  --check-prefixes=CHECK,NO-CLONES \
  "$FIXTURE" \
  <"$TEMP/repeated.ll"

if [[ "$(grep -Fc 'define private swiftcc i64 @integerTarget.hotfix_original' "$TEMP/repeated.ll")" != "1" ]]; then
  echo "error: repeated pass instrumentation was not idempotent" >&2
  exit 1
fi
if ! grep -Fq '@llvm.compiler.used = appending global [18 x ptr]' "$TEMP/repeated.ll"; then
  echo "error: repeated pass duplicated retained function entries" >&2
  exit 1
fi
if [[ "$(grep -Fc 'section "__DATA,__hotfix"' "$TEMP/repeated.ll")" != "9" ]]; then
  echo "error: repeated pass duplicated or omitted hotfix descriptors" >&2
  exit 1
fi
if ! grep -Fq '@llvm.used = appending global [9 x ptr]' "$TEMP/repeated.ll"; then
  echo "error: repeated pass duplicated retained descriptor entries" >&2
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

xcrun swiftc \
  -O \
  -emit-bc \
  -parse-as-library \
  -module-name HotfixReceiverPassFixture \
  "$SWIFT_FIXTURE" \
  -o "$TEMP/swift.input.bc"
"$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  "$TEMP/swift.input.bc" \
  -o "$TEMP/swift.transformed.bc" \
  2>"$TEMP/swift.stderr"
"$LLVM_DIS" "$TEMP/swift.transformed.bc" -o "$TEMP/swift.transformed.ll"
grep -Eq '^define swiftcc i64 .*instanceTarget.*\(i64 [^,]+, ptr [^)]*swiftself' "$TEMP/swift.transformed.ll"
grep -Eq '^define private swiftcc i64 .*instanceTarget.*\.hotfix_original"?\(i64 [^,]+, ptr [^)]*swiftself' "$TEMP/swift.transformed.ll"
grep -Eq 'call i1 @ir_hotfix_invoke\(i64 [^,]+, i64 [^,]+, ptr [^,]+, ptr [^,]+, i32 1, ptr %[^,]+, ptr %[^)]+\)' "$TEMP/swift.transformed.ll"
grep -Eq 'private constant %struct\.ir_hotfix_descriptor \{ i64 [^,]+, i64 [^,]+, i8 1, i32 1, i8 1, ptr [^,]+, ptr [^}]+\}, section "__DATA,__hotfix"' "$TEMP/swift.transformed.ll"
