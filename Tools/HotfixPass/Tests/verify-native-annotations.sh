#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WRAPPER="$ROOT/Tools/HotfixPass/clang-hotfix"
PATCH_BUILDER="$ROOT/Tools/HotfixPass/clang-patch-build"
PACKAGE_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixPackageTool"
VM_TESTS="$ROOT/Tools/HotfixPass/.build/HFIRVMTests"
FIXTURES="$ROOT/Tools/HotfixPass/Tests/fixtures"
MANIFEST="$ROOT/PatchExamples/HotfixTargetManifest.json"
MACOS_SDK="$(xcrun --sdk macosx --show-sdk-path)"
MACOS_TARGET="$(uname -m)-apple-macosx$(sw_vers -productVersion | awk -F. '{ print $1 ".0" }')"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-native-annotations.XXXXXX")"

cleanup() {
  find "$TEMP" -type f -delete
  find "$TEMP" -depth -type d -empty -delete
}
trap cleanup EXIT

compile_annotated() {
  local language="$1"
  local source="$2"
  local output="$3"
  local language_flags=(-x "$language")
  if [[ "$language" == "c++" ]]; then
    language_flags+=(-std=c++20 -fno-exceptions -fno-rtti)
  fi
  IR_HOTFIX_INSTRUMENTATION=YES \
  IR_HOTFIX_PLUGIN_PATH="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib" \
  IR_HOTFIX_CLANG_PLUGIN_PATH="$ROOT/Tools/HotfixPass/.build/libHotfixClangPlugin.dylib" \
  IR_HOTFIX_TARGET_MANIFEST_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixManifestTool" \
    "$WRAPPER" "${language_flags[@]}" \
      -isysroot "$MACOS_SDK" -target "$MACOS_TARGET" \
      -c "$source" -o "$output"
}

compile_annotated c "$FIXTURES/native_annotated_c.c" "$TEMP/native-c.o"
test -f "$TEMP/native-c.o"
test -f "$TEMP/native-c.o.hotfix-targets.json"
grep -Fq '"symbol": "hotfix_example_c_add"' \
  "$TEMP/native-c.o.hotfix-targets.json"
grep -Fq '"receiverKind": "none"' \
  "$TEMP/native-c.o.hotfix-targets.json"

printf '%s\n' \
  '-x' \
  'c' \
  '-isysroot' \
  "$MACOS_SDK" \
  '-target' \
  "$MACOS_TARGET" \
  '-c' \
  "$FIXTURES/native_annotated_c.c" \
  '-o' \
  "$TEMP/native-response.o" \
  >"$TEMP/native-response.rsp"
IR_HOTFIX_INSTRUMENTATION=YES \
IR_HOTFIX_PLUGIN_PATH="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib" \
IR_HOTFIX_CLANG_PLUGIN_PATH="$ROOT/Tools/HotfixPass/.build/libHotfixClangPlugin.dylib" \
IR_HOTFIX_TARGET_MANIFEST_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixManifestTool" \
  "$WRAPPER" "@$TEMP/native-response.rsp"
test -f "$TEMP/native-response.o.hotfix-targets.json"
grep -Fq '"symbol": "hotfix_example_c_add"' \
  "$TEMP/native-response.o.hotfix-targets.json"

compile_annotated c++ "$FIXTURES/native_annotated_cxx.cpp" "$TEMP/native-cxx.o"
test -f "$TEMP/native-cxx.o"
test -f "$TEMP/native-cxx.o.hotfix-targets.json"
grep -Fq '"symbol": "_ZN12HFCalculator8multiplyEx"' \
  "$TEMP/native-cxx.o.hotfix-targets.json"
grep -Fq '"receiverKind": "native"' \
  "$TEMP/native-cxx.o.hotfix-targets.json"

IR_HOTFIX_INSTRUMENTATION=YES \
IR_HOTFIX_PATCH_MODE=1 \
IR_HOTFIX_BASELINE_MANIFEST="$MANIFEST" \
IR_HOTFIX_PLUGIN_PATH="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib" \
IR_HOTFIX_CLANG_PLUGIN_PATH="$ROOT/Tools/HotfixPass/.build/libHotfixClangPlugin.dylib" \
IR_HOTFIX_PATCH_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixPatchTool" \
IR_HOTFIX_TARGET_MANIFEST_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixManifestTool" \
  "$WRAPPER" -x c -isysroot "$MACOS_SDK" -target "$MACOS_TARGET" \
    -c "$FIXTURES/native_annotated_c.c" \
    -o "$TEMP/native-c-patch.o"
test -f "$TEMP/native-c-patch.o"
test -f "$TEMP/native-c-patch.o.hotfix-patches/0x509615878e53c0ac.irpatch"
test -f "$TEMP/native-c-patch.o.hotfix-patches/0x509615878e53c0ac.hfpatch"
"$VM_TESTS" --invoke-i64 \
  "$TEMP/native-c-patch.o.hotfix-patches/0x509615878e53c0ac.hfpatch" \
  10 210

if compile_annotated c++ \
  "$FIXTURES/native_annotated_virtual.cpp" \
  "$TEMP/native-virtual.o" \
  2>"$TEMP/native-virtual.stderr"; then
  echo "error: native annotation compiler accepted a virtual method" >&2
  exit 1
fi
grep -Fq 'virtual C++ methods are not supported hotfix targets' \
  "$TEMP/native-virtual.stderr"
test ! -e "$TEMP/native-virtual.o"
test ! -e "$TEMP/native-virtual.o.hotfix-targets.json"

"$PATCH_BUILDER" \
  --language c \
  --manifest "$MANIFEST" \
  --source "$FIXTURES/native_annotated_c.c" \
  --output "$TEMP/native-c.hfpatch"
"$PACKAGE_TOOL" verify "$TEMP/native-c.hfpatch"
"$VM_TESTS" --invoke-i64 "$TEMP/native-c.hfpatch" 10 210

"$PATCH_BUILDER" \
  --language cxx \
  --manifest "$MANIFEST" \
  --source "$FIXTURES/native_annotated_cxx.cpp" \
  --output "$TEMP/native-cxx.hfpatch"
"$PACKAGE_TOOL" verify "$TEMP/native-cxx.hfpatch"
"$VM_TESTS" --invoke-native-i64 "$TEMP/native-cxx.hfpatch" 8 40

echo "[HotfixNativeAnnotationTests] all checks passed"
