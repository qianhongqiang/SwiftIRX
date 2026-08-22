#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WRAPPER="$ROOT/Tools/HotfixPass/swiftc-hotfix"
PLUGIN="${IR_HOTFIX_PLUGIN_PATH:-$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib}"
FIXTURES="$ROOT/Tools/HotfixPass/Tests/fixtures"
LLVM_ROOT="/opt/homebrew/opt/llvm@19/bin"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
TARGET="$(uname -m)-apple-macosx$(sw_vers -productVersion | awk -F. '{ print $1 ".0" }')"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-wrapper.XXXXXX")"

cleanup() {
  find "$TEMP" -type f -delete
  find "$TEMP" -depth -type d -empty -delete
}
trap cleanup EXIT

if [[ ! -x "$WRAPPER" ]]; then
  echo "error: Swift compiler wrapper not found at $WRAPPER" >&2
  exit 1
fi
if [[ ! -f "$PLUGIN" ]]; then
  echo "error: pass plugin not found at $PLUGIN" >&2
  exit 1
fi
"$WRAPPER" --version | grep -Fq 'Apple Swift version 6.2.4'

(
  cd "$TEMP"
  xcrun clang -x objective-c++ -std=c++20 \
    -I "$ROOT/SDK/IRHotfixSDK/ABI" \
    -c "$FIXTURES/runtime_stub.c" -o runtime_stub.o
  IR_HOTFIX_PLUGIN_PATH="$PLUGIN" "$WRAPPER" \
    -Onone \
    -g \
    -Xfrontend -stack-check \
    -disable-batch-mode \
    -module-name HotfixWrapperFixture \
    -sdk "$SDK" \
    "$FIXTURES/wrapper_scalar.swift" \
    "$FIXTURES/wrapper_main.swift" \
    -c
  xcrun swiftc -sdk "$SDK" \
    wrapper_scalar.o wrapper_main.o runtime_stub.o \
    -o wrapper-smoke
  ./wrapper-smoke >native-output.txt
)

grep -qx '42' "$TEMP/native-output.txt"
"$LLVM_ROOT/llvm-nm" "$TEMP/wrapper_scalar.o" >"$TEMP/scalar-symbols.txt"
grep -Fq 'wrapperAdd' "$TEMP/scalar-symbols.txt"
grep -Fq '.hotfix_original' "$TEMP/scalar-symbols.txt"
grep -Fq '_hf_vm_invoke' "$TEMP/scalar-symbols.txt"
test -f "$TEMP/wrapper_scalar.o.hotfix-targets.json"
grep -Fq '"schemaVersion": 1' "$TEMP/wrapper_scalar.o.hotfix-targets.json"
grep -Fq 'wrapperAdd' "$TEMP/wrapper_scalar.o.hotfix-targets.json"
"$LLVM_ROOT/llvm-objdump" --macho --section-headers \
  "$TEMP/wrapper_scalar.o" >"$TEMP/scalar-sections.txt"
grep -Fq '__hotfix' "$TEMP/scalar-sections.txt"

mkdir -p "$TEMP/fixture directory" "$TEMP/responses"
cp "$FIXTURES/wrapper_scalar.swift" \
  "$TEMP/fixture directory/wrapper scalar.swift"
printf '%s\n' \
  '-c' \
  '-parse-as-library' \
  '-primary-file' \
  '"fixture directory/wrapper scalar.swift"' \
  '-module-name' \
  'HotfixFrontendResponseFixture' \
  '-target' \
  "$TARGET" \
  '-sdk' \
  "\"$SDK\"" \
  '-o' \
  '"response object.o"' \
  >"$TEMP/responses/compile args.rsp"
printf '%s\n' '@"responses/compile args.rsp"' \
  >"$TEMP/responses/frontend.rsp"
(
  cd "$TEMP"
  IR_HOTFIX_PLUGIN_PATH="$PLUGIN" "$WRAPPER" \
    -frontend @responses/frontend.rsp
)
"$LLVM_ROOT/llvm-nm" "$TEMP/response object.o" \
  >"$TEMP/response-symbols.txt"
grep -Fq '.hotfix_original' "$TEMP/response-symbols.txt"
grep -Fq '_hf_vm_invoke' "$TEMP/response-symbols.txt"
test -f "$TEMP/response object.o.hotfix-targets.json"
grep -Fq 'wrapperAdd' "$TEMP/response object.o.hotfix-targets.json"
"$LLVM_ROOT/llvm-objdump" --macho --section-headers \
  "$TEMP/response object.o" >"$TEMP/response-sections.txt"
grep -Fq '__hotfix' "$TEMP/response-sections.txt"

printf '%s\n' '@"responses/cycle-b.rsp"' >"$TEMP/responses/cycle-a.rsp"
printf '%s\n' '@"responses/cycle-a.rsp"' >"$TEMP/responses/cycle-b.rsp"
if (
  cd "$TEMP"
  IR_HOTFIX_PLUGIN_PATH="$PLUGIN" "$WRAPPER" \
    -frontend @responses/cycle-a.rsp 2>cycle.stderr
); then
  echo "error: response file cycle unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq '[HotfixWrapper] error: response file cycle:' "$TEMP/cycle.stderr"

if (
  cd "$TEMP"
  IR_HOTFIX_PLUGIN_PATH="$PLUGIN" "$WRAPPER" \
    -frontend @responses/missing.rsp 2>missing.stderr
); then
  echo "error: unreadable response file unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq '[HotfixWrapper] error: response file is not readable:' \
  "$TEMP/missing.stderr"

IR_HOTFIX_PLUGIN_PATH="$PLUGIN" "$WRAPPER" \
  -Onone \
  -parse-as-library \
  -c "$FIXTURES/Hotfix.swift" \
  -module-name HotfixExcludedFixture \
  -sdk "$SDK" \
  -o "$TEMP/Hotfix.o"
"$LLVM_ROOT/llvm-nm" "$TEMP/Hotfix.o" >"$TEMP/excluded-symbols.txt"
grep -Fq 'excludedAdd' "$TEMP/excluded-symbols.txt"
if grep -Fq '.hotfix_original' "$TEMP/excluded-symbols.txt"; then
  echo "error: excluded Hotfix.swift was instrumented" >&2
  exit 1
fi
if [[ -e "$TEMP/Hotfix.o.hotfix-targets.json" ]]; then
  echo "error: excluded Hotfix.swift emitted a target manifest" >&2
  exit 1
fi
if "$LLVM_ROOT/llvm-objdump" --macho --section-headers "$TEMP/Hotfix.o" |
  grep -Fq '__hotfix'; then
  echo "error: excluded Hotfix.swift contains a hotfix descriptor section" >&2
  exit 1
fi
