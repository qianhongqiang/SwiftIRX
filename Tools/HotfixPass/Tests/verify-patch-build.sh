#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILDER="$ROOT/Tools/HotfixPass/swift-patch-build"
PATCH_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixPatchTool"
MACRO_PACKAGE="$ROOT/Tools/HotfixMacros"
ANNOTATION="$ROOT/SDK/IRHotfixSDK/Runtime/HotfixPatchAnnotation.swift"
SWIFT="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swift"
SWIFTC="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc"
MANIFEST="$ROOT/Tools/HotfixPass/Tests/fixtures/target_manifest.json"
SETUP_UI_MANIFEST="$ROOT/Tools/HotfixPass/Tests/fixtures/setup_ui_manifest.json"
FIXTURES="$ROOT/Tools/HotfixPass/Tests/fixtures"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-patch-build-tests.XXXXXX")"

cleanup() {
  find "$TEMP" -type f -delete
  find "$TEMP" -depth -type d -empty -delete
}
trap cleanup EXIT

[[ -x "$BUILDER" ]] || {
  echo "error: Swift patch builder is not executable: $BUILDER" >&2
  exit 1
}

"$BUILDER" \
  --manifest "$MANIFEST" \
  --target integerTarget \
  --source "$FIXTURES/patch_integer.swift" \
  --output "$TEMP/integer.irpatch"
grep -Fq 'define dso_local swiftcc i64 @integerTarget(i64' "$TEMP/integer.irpatch"
grep -Fq 'add i64' "$TEMP/integer.irpatch"
grep -Fq 'entry:' "$TEMP/integer.irpatch"
if [[ "$(grep -c '^define ' "$TEMP/integer.irpatch")" != "1" ]]; then
  echo "error: generated patch does not contain exactly one function" >&2
  exit 1
fi

"$BUILDER" \
  --manifest "$MANIFEST" \
  --target instanceTarget \
  --source "$FIXTURES/patch_receiver.swift" \
  --output "$TEMP/receiver.irpatch"
grep -Fq 'define dso_local swiftcc i64 @instanceTarget(ptr' "$TEMP/receiver.irpatch"

"$BUILDER" \
  --manifest "$MANIFEST" \
  --target integerTarget \
  --source "$FIXTURES/patch_integer.swift" \
  --output "$TEMP/integer.hfpatch"
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" verify "$TEMP/integer.hfpatch"
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" dump \
  "$TEMP/integer.hfpatch" >"$TEMP/integer.hfir.txt"
grep -Fq 'add.i64' "$TEMP/integer.hfir.txt"
if grep -Eq '(^|[^[:alpha:]])(define|declare|llvm\.)|\$s[0-9]' "$TEMP/integer.hfir.txt"; then
  echo "error: lowered integer patch leaked compiler IR" >&2
  exit 1
fi

"$BUILDER" \
  --manifest "$MANIFEST" \
  --target integerTarget \
  --source "$FIXTURES/patch_native_swift_call.swift" \
  --output "$TEMP/native-swift.hfpatch"
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" dump \
  "$TEMP/native-swift.hfpatch" >"$TEMP/native-swift.hfir.txt"
grep -Fq 'native-swift Swift.$s21ReleasedHostFunctions12doubleValueyS2iF' \
  "$TEMP/native-swift.hfir.txt"
grep -Fq 'host.call' "$TEMP/native-swift.hfir.txt"

if "$BUILDER" \
  --manifest "$MANIFEST" \
  --target integerTarget \
  --source "$FIXTURES/patch_native_too_many_arguments.swift" \
  --output "$TEMP/native-too-many.hfpatch" \
  2>"$TEMP/native-too-many.stderr"; then
  echo "error: patch builder accepted an oversized native host call" >&2
  exit 1
fi
grep -Fq 'native call exceeds maximum host argument count' \
  "$TEMP/native-too-many.stderr"
test ! -e "$TEMP/native-too-many.hfpatch"

"$PATCH_TOOL" lower-hfir \
  "$MANIFEST" \
  instanceTarget \
  "$FIXTURES/patch_native_cxx_method.ll" \
  "$TEMP/native-cxx-method.hfpatch"
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" dump \
  "$TEMP/native-cxx-method.hfpatch" >"$TEMP/native-cxx-method.hfir.txt"
grep -Fq 'native-cxx C++._ZN16ReleasedCounter8multiplyEl' \
  "$TEMP/native-cxx-method.hfir.txt"
grep -Fq '(receiver: handle, i64)' "$TEMP/native-cxx-method.hfir.txt"
grep -Fq 'host.call' "$TEMP/native-cxx-method.hfir.txt"

"$BUILDER" \
  --manifest "$SETUP_UI_MANIFEST" \
  --target setupUI \
  --source "$FIXTURES/patch_setup_ui.swift" \
  --output "$TEMP/setup-ui.hfpatch"
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" verify "$TEMP/setup-ui.hfpatch"
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" dump \
  "$TEMP/setup-ui.hfpatch" >"$TEMP/setup-ui.hfir.txt"
grep -Fq 'object.construct' "$TEMP/setup-ui.hfir.txt"
grep -Fq 'object.invoke' "$TEMP/setup-ui.hfir.txt"
grep -Fq 'UILabel.setText:' "$TEMP/setup-ui.hfir.txt"
if [[ "$(grep -Fc '"hello"' "$TEMP/setup-ui.hfir.txt")" != "1" ]]; then
  echo "error: Swift string literal was materialized more than once" >&2
  exit 1
fi
if grep -Eq '(^|[^[:alpha:]])(define|declare|llvm\.)|\$s[0-9]' "$TEMP/setup-ui.hfir.txt"; then
  echo "error: lowered UIKit patch leaked compiler IR" >&2
  exit 1
fi

if "$BUILDER" \
  --manifest "$MANIFEST" \
  --target integerTarget \
  --source "$FIXTURES/patch_invalid_signature.swift" \
  --output "$TEMP/invalid.irpatch" \
  2>"$TEMP/invalid.stderr"; then
  echo "error: patch builder accepted an incompatible Swift signature" >&2
  exit 1
fi
grep -Fq 'return type does not match target' "$TEMP/invalid.stderr"
test ! -e "$TEMP/invalid.irpatch"

if "$BUILDER" \
  --manifest "$MANIFEST" \
  --target Target \
  --source "$FIXTURES/patch_integer.swift" \
  --output "$TEMP/ambiguous.irpatch" \
  2>"$TEMP/ambiguous.stderr"; then
  echo "error: patch builder accepted an ambiguous target query" >&2
  exit 1
fi
grep -Fq "target query 'Target' is ambiguous" "$TEMP/ambiguous.stderr"

if "$BUILDER" \
  --manifest "$MANIFEST" \
  --target integerTarget \
  --source "$FIXTURES/patch_local_helper.swift" \
  --output "$TEMP/helper.irpatch" \
  2>"$TEMP/helper.stderr"; then
  echo "error: patch builder accepted a non-inlined local helper" >&2
  exit 1
fi
grep -Fq 'inline the helper before building the patch' "$TEMP/helper.stderr"

"$SWIFT" build \
  --package-path "$MACRO_PACKAGE" \
  --scratch-path "$TEMP/MacroBuild" \
  --configuration release \
  --product IRHotfixMacrosPlugin
macro_bin_directory="$(
  "$SWIFT" build \
    --package-path "$MACRO_PACKAGE" \
    --scratch-path "$TEMP/MacroBuild" \
    --configuration release \
    --show-bin-path
)"
macro_plugin="$macro_bin_directory/IRHotfixMacrosPlugin-tool"
test -x "$macro_plugin"
test -x "$PATCH_TOOL"

sdk_path="$(xcrun --sdk iphonesimulator --show-sdk-path)"
"$SWIFTC" \
  -emit-bc \
  -parse-as-library \
  -whole-module-optimization \
  -Onone \
  -gnone \
  -D IR_HOTFIX_PATCH_BUILD \
  -load-plugin-executable "$macro_plugin#IRHotfixMacrosPlugin" \
  -module-name IRPatchAnnotationTests \
  -sdk "$sdk_path" \
  -target arm64-apple-ios26.2-simulator \
  -module-cache-path "$TEMP/ModuleCache" \
  "$ANNOTATION" \
  "$FIXTURES/annotated_patch.swift" \
  -o "$TEMP/annotated.bc"

mkdir "$TEMP/annotated-output"
"$PATCH_TOOL" extract-annotated \
  "$MANIFEST" \
  "$TEMP/annotated.bc" \
  "$TEMP/annotated-output"
annotated_patch="$TEMP/annotated-output/0x37465577a37332e8.irpatch"
annotated_binary_patch="$TEMP/annotated-output/0x37465577a37332e8.hfpatch"
test -f "$annotated_patch"
test -f "$annotated_binary_patch"
grep -Fq 'define dso_local swiftcc i64 @integerTarget(i64' "$annotated_patch"
grep -Fq 'add i64' "$annotated_patch"
grep -Fq ', 23' "$annotated_patch"
if grep -Fq '__ir_hotfix_patch_anchor_' "$annotated_patch"; then
  echo "error: generated Patch contains the macro anchor" >&2
  exit 1
fi
"$ROOT/Tools/HotfixPass/.build/HotfixPackageTool" verify "$annotated_binary_patch"

echo "[HotfixPatchTests] all checks passed"
