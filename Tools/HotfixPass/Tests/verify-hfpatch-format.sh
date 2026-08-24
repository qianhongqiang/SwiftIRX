#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$ROOT/Tools/HotfixPass/.build/HotfixPackageTool"
FORMAT_TESTS="$ROOT/Tools/HotfixPass/.build/HFIRFormatTests"
VM_TESTS="$ROOT/Tools/HotfixPass/.build/HFIRVMTests"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-hfir-format.XXXXXX")"

cleanup() {
  rm -rf "$TEMP"
}
trap cleanup EXIT

[[ -x "$TOOL" ]] || { echo "error: missing $TOOL" >&2; exit 1; }
[[ -x "$FORMAT_TESTS" ]] || { echo "error: missing $FORMAT_TESTS" >&2; exit 1; }
[[ -x "$VM_TESTS" ]] || { echo "error: missing $VM_TESTS" >&2; exit 1; }

"$FORMAT_TESTS"
"$VM_TESTS"
"$TOOL" create-example "$TEMP/first.hfpatch"
"$TOOL" create-example "$TEMP/second.hfpatch"
cmp "$TEMP/first.hfpatch" "$TEMP/second.hfpatch"
"$TOOL" verify "$TEMP/first.hfpatch"
"$TOOL" dump "$TEMP/first.hfpatch" >"$TEMP/dump.txt"

grep -Fq 'hfir-version 2' "$TEMP/dump.txt"
grep -Fq 'abi-version 3' "$TEMP/dump.txt"
grep -Fq 'object.construct' "$TEMP/dump.txt"
grep -Fq 'object.invoke' "$TEMP/dump.txt"
grep -Fq 'string.constant' "$TEMP/dump.txt"
grep -Fq 'UILabel.setText:' "$TEMP/dump.txt"
if grep -Eq '(^|[^[:alpha:]])(define|declare|llvm\.)|\$s[0-9]' "$TEMP/dump.txt"; then
  echo "error: .hfpatch dump leaked LLVM IR or a Swift mangled symbol" >&2
  exit 1
fi

cp "$TEMP/first.hfpatch" "$TEMP/corrupt.hfpatch"
printf '\x01' | dd of="$TEMP/corrupt.hfpatch" bs=1 seek=80 conv=notrunc status=none
if "$TOOL" verify "$TEMP/corrupt.hfpatch" 2>"$TEMP/corrupt.stderr"; then
  echo "error: verifier accepted a corrupted .hfpatch" >&2
  exit 1
fi
grep -Fq 'payload integrity hash does not match' "$TEMP/corrupt.stderr"

echo "verify-hfpatch-format passed"
