#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$ROOT/Tools/HotfixPass/.build/HotfixAdapterTool"
FIXTURES="$ROOT/Tools/HotfixPass/Tests/fixtures"
HOST_ADAPTERS="$ROOT/SDK/IRHotfixSDK/HostAdapter"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-host-adapters.XXXXXX")"

cleanup() {
  find "$TEMP" -type f -delete
  find "$TEMP" -depth -type d -empty -delete
}
trap cleanup EXIT

test -x "$TOOL"
mkdir "$TEMP/Generated"
"$TOOL" generate \
  "$FIXTURES/host_adapters.json" \
  "$TEMP/Generated/HotfixGeneratedHostAdapters.swift" \
  "$TEMP/Generated/HFGeneratedHostAdapters.mm" \
  "$TEMP/HotfixHostAdapterManifest.json"

grep -Fq 'doubleValue(Int(bitPattern:' \
  "$TEMP/Generated/HotfixGeneratedHostAdapters.swift"
grep -Fq 'irhotfix::host::registerFunction("ir_test_c_increment"' \
  "$TEMP/Generated/HFGeneratedHostAdapters.mm"
grep -Fq 'irhotfix::host::registerMethod("_ZN13IRTestCounter8multiplyEl"' \
  "$TEMP/Generated/HFGeneratedHostAdapters.mm"
grep -Fq '"language": "swift"' "$TEMP/HotfixHostAdapterManifest.json"
grep -Fq '"language": "c"' "$TEMP/HotfixHostAdapterManifest.json"
grep -Fq '"language": "cxx"' "$TEMP/HotfixHostAdapterManifest.json"
grep -Eq '"importID": "0x[0-9a-f]{16}"' \
  "$TEMP/HotfixHostAdapterManifest.json"
grep -Eq '"signatureID": "0x[0-9a-f]{16}"' \
  "$TEMP/HotfixHostAdapterManifest.json"
jq empty "$TEMP/HotfixHostAdapterManifest.json"

xcrun --sdk macosx clang++ \
  -std=c++20 \
  -fsyntax-only \
  -x objective-c++ \
  -I "$HOST_ADAPTERS/Generated" \
  -I "$HOST_ADAPTERS" \
  -I "$FIXTURES" \
  "$TEMP/Generated/HFGeneratedHostAdapters.mm"
