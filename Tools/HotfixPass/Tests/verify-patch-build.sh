#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILDER="$ROOT/Tools/HotfixPass/swift-patch-build"
MANIFEST="$ROOT/Tools/HotfixPass/Tests/fixtures/target_manifest.json"
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

echo "[HotfixPatchTests] all checks passed"
