#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
TARGET_MANIFEST_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixManifestTool"
FIRST_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture.swift"
SECOND_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture_second.swift"
MANIFEST_GENERATOR="$ROOT/Tools/HotfixPass/generate-class-receiver-manifest.sh"
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
if [[ ! -x "$TARGET_MANIFEST_TOOL" ]]; then
  echo "error: target manifest tool not found at $TARGET_MANIFEST_TOOL" >&2
  exit 1
fi
if [[ ! -x "$MANIFEST_GENERATOR" ]]; then
  echo "error: receiver manifest generator not found at $MANIFEST_GENERATOR" >&2
  exit 1
fi

transform_swift_module() {
  local fixture="$1"
  local module_name="$2"
  local output_name="$3"

  xcrun swiftc \
    -O \
    -emit-bc \
    -parse-as-library \
    -module-name "$module_name" \
    "$fixture" \
    -o "$TEMP/$output_name.input.bc"

  "$MANIFEST_GENERATOR" \
    "$TEMP/$output_name.input.bc" \
    "$TEMP/$output_name.manifest"

  IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$TEMP/$output_name.manifest" \
    "$LLVM_ROOT/opt" \
    -load-pass-plugin "$PLUGIN" \
    -passes=hotfix-instrument \
    -verify-each \
    "$TEMP/$output_name.input.bc" \
    -o "$TEMP/$output_name.transformed.bc"
}

transform_swift_module "$FIRST_FIXTURE" HotfixFixtureOne first
transform_swift_module "$SECOND_FIXTURE" HotfixFixtureTwo second

"$LLVM_ROOT/llvm-dis" \
  "$TEMP/first.transformed.bc" \
  -o "$TEMP/first.transformed.ll"
"$LLVM_ROOT/llvm-dis" \
  "$TEMP/second.transformed.bc" \
  -o "$TEMP/second.transformed.ll"
grep -Fq '@hotfix_pass_loaded = private constant' "$TEMP/first.transformed.ll"
grep -Fq 'hotfix-pass-loaded' "$TEMP/first.transformed.ll"
grep -Fq '@hotfix_pass_loaded = private constant' "$TEMP/second.transformed.ll"
grep -Fq 'hotfix-pass-loaded' "$TEMP/second.transformed.ll"
grep -Eq '^define private swiftcc i64 .*instanceTarget.*\.hotfix_original' \
  "$TEMP/first.transformed.ll"

"$LLVM_ROOT/llvm-link" \
  "$TEMP/first.transformed.bc" \
  "$TEMP/second.transformed.bc" \
  -o "$TEMP/linked.bc"

"$TARGET_MANIFEST_TOOL" extract \
  "$TEMP/linked-targets.json" \
  "$TEMP/linked.bc"
grep -Fq 'HotfixFixtureOne' "$TEMP/linked-targets.json"
grep -Fq 'HotfixFixtureTwo' "$TEMP/linked-targets.json"

"$LLVM_ROOT/llvm-dis" \
  "$TEMP/linked.bc" \
  -o "$TEMP/linked.ll"

grep -Fq "HotfixFixtureOne" "$TEMP/linked.ll"
grep -Fq "HotfixFixtureTwo" "$TEMP/linked.ll"
if [[ "$(grep -Fc 'call i32 @hf_vm_invoke' "$TEMP/linked.ll")" -lt 2 ]]; then
  echo "error: linked Swift modules did not retain both hotfix trampolines" >&2
  exit 1
fi
if [[ "$(grep -Ec '^define private swiftcc .*hotfix_original' "$TEMP/linked.ll")" -lt 2 ]]; then
  echo "error: linked Swift modules did not retain both native clones" >&2
  exit 1
fi
