#!/bin/bash

set -euo pipefail

LLVM_NM="/opt/homebrew/opt/llvm@19/bin/llvm-nm"

if [[ "$#" != 2 ]]; then
  echo "usage: $0 <input-bitcode> <output-manifest>" >&2
  exit 2
fi

INPUT="$1"
OUTPUT="$2"
OUTPUT_DIRECTORY="$(dirname "$OUTPUT")"
TEMP=""
OUTPUT_TEMP=""

cleanup() {
  if [[ -n "$TEMP" ]]; then
    rm -rf "$TEMP"
  fi
  if [[ -n "$OUTPUT_TEMP" ]]; then
    rm -f "$OUTPUT_TEMP"
  fi
}
trap cleanup EXIT

if [[ ! -x "$LLVM_NM" ]]; then
  echo "error: required Homebrew llvm-nm not found at $LLVM_NM" >&2
  exit 1
fi
if ! xcrun --find swift-demangle >/dev/null 2>&1; then
  echo "error: required Apple swift-demangle was not found by xcrun" >&2
  exit 1
fi
if [[ ! -r "$INPUT" || ! -f "$INPUT" ]]; then
  echo "error: input bitcode is not readable: $INPUT" >&2
  exit 1
fi
if [[ ! -d "$OUTPUT_DIRECTORY" || ! -w "$OUTPUT_DIRECTORY" ]]; then
  echo "error: output directory is not writable: $OUTPUT_DIRECTORY" >&2
  exit 1
fi

TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-receiver-manifest.XXXXXX")"
if ! OUTPUT_TEMP="$(
  mktemp "$OUTPUT_DIRECTORY/.$(basename "$OUTPUT").tmp.XXXXXX"
)"; then
  echo "error: could not create temporary output in: $OUTPUT_DIRECTORY" >&2
  exit 1
fi

if ! "$LLVM_NM" --defined-only --just-symbol-name "$INPUT" >"$TEMP/nm.txt"; then
  echo "error: llvm-nm could not read input bitcode: $INPUT" >&2
  exit 1
fi

awk '
  {
    symbol = $0
    if (symbol ~ /^_\$s/)
      symbol = substr(symbol, 2)
    if (symbol ~ /^\$s/)
      print symbol
  }
' "$TEMP/nm.txt" | LC_ALL=C sort -u >"$TEMP/symbols.txt"

if ! xcrun swift-demangle --expand \
  <"$TEMP/symbols.txt" \
  >"$TEMP/demangled.txt"; then
  echo "error: swift-demangle could not expand symbols from: $INPUT" >&2
  exit 1
fi

awk '
  function indentation(text) {
    match(text, /^ */)
    return RLENGTH
  }

  function node_kind(text, value) {
    value = text
    sub(/^ *kind=/, "", value)
    sub(/,.*/, "", value)
    return value
  }

  function is_callable(kind) {
    return kind == "Function" || kind == "Getter" || kind == "Setter" ||
           kind == "ModifyAccessor" || kind == "ReadAccessor" ||
           kind == "WillSet" || kind == "DidSet"
  }

  function is_nominal(kind) {
    return kind == "Class" || kind == "Structure" || kind == "Enum" ||
           kind == "Protocol" || kind == "Actor"
  }

  function process_block(    context, current, kind, root, root_index) {
    if (symbol == "")
      return

    root = ""
    root_index = 0
    for (current = 1; current <= line_count; ++current) {
      if (indentation(lines[current]) == 2 && lines[current] ~ /^  kind=/) {
        root = node_kind(lines[current])
        root_index = current
        break
      }
    }

    if (is_callable(root)) {
      context = 0
      for (current = root_index + 1; current <= line_count; ++current) {
        kind = node_kind(lines[current])
        if (kind == "Identifier")
          break
        if (is_nominal(kind)) {
          context = current
          break
        }
      }
      if (context != 0 && node_kind(lines[context]) == "Class")
        candidates[symbol] = 1
    }
  }

  /^Demangling for / {
    process_block()
    symbol = substr($0, length("Demangling for ") + 1)
    delete lines
    line_count = 0
    next
  }

  {
    lines[++line_count] = $0
  }

  END {
    process_block()
    for (candidate in candidates)
      print candidate
  }
' "$TEMP/demangled.txt" | LC_ALL=C sort -u >"$OUTPUT_TEMP"

if ! mv "$OUTPUT_TEMP" "$OUTPUT"; then
  echo "error: could not atomically write receiver manifest: $OUTPUT" >&2
  exit 1
fi
OUTPUT_TEMP=""
