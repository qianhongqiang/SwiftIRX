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

  function subtree_key(node_index, base, key, current) {
    base = indentation(lines[node_index])
    key = ""
    for (current = node_index; current <= line_count; ++current) {
      if (current > node_index && indentation(lines[current]) <= base)
        break
      key = key substr(lines[current], base + 1) "\034"
    }
    return key
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

  function is_conformance_record(kind) {
    return kind == "ProtocolConformanceDescriptor" ||
           kind == "ProtocolConformance"
  }

  function direct_child(node_index, expected_kind, base, current) {
    base = indentation(lines[node_index])
    for (current = node_index + 1; current <= line_count; ++current) {
      if (indentation(lines[current]) <= base)
        break
      if (indentation(lines[current]) == base + 2 &&
          node_kind(lines[current]) == expected_kind)
        return current
    }
    return 0
  }

  function is_swift_actor_protocol(protocol_index, base, current,
                                   found_actor, found_swift, kind) {
    base = indentation(lines[protocol_index])
    found_actor = 0
    found_swift = 0
    for (current = protocol_index + 1; current <= line_count; ++current) {
      if (indentation(lines[current]) <= base)
        break
      if (indentation(lines[current]) != base + 2)
        continue
      kind = node_kind(lines[current])
      if (kind == "Module" && lines[current] ~ /text="Swift"/)
        found_swift = 1
      if (kind == "Identifier" && lines[current] ~ /text="Actor"/)
        found_actor = 1
    }
    return found_swift && found_actor
  }

  function actor_subject(conformance_index, base, class_node, current,
                         protocol_node, protocol_type, subject_type,
                         type_count) {
    base = indentation(lines[conformance_index])
    type_count = 0
    subject_type = 0
    protocol_type = 0
    for (current = conformance_index + 1;
         current <= line_count;
         ++current) {
      if (indentation(lines[current]) <= base)
        break
      if (indentation(lines[current]) == base + 2 &&
          node_kind(lines[current]) == "Type") {
        ++type_count
        if (type_count == 1)
          subject_type = current
        else if (type_count == 2)
          protocol_type = current
      }
    }
    if (subject_type == 0 || protocol_type == 0)
      return 0

    class_node = direct_child(subject_type, "Class")
    protocol_node = direct_child(protocol_type, "Protocol")
    if (class_node == 0 || protocol_node == 0 ||
        !is_swift_actor_protocol(protocol_node))
      return 0
    return class_node
  }

  function process_block(    actor_class, context, current, kind, root,
                              root_index) {
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
        candidates[symbol] = subtree_key(context)
    }

    if (is_conformance_record(root)) {
      for (current = 1; current <= line_count; ++current) {
        if (node_kind(lines[current]) == "ProtocolConformance") {
          actor_class = actor_subject(current)
          if (actor_class != 0)
            actors[subtree_key(actor_class)] = 1
        }
      }
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
    for (candidate in candidates) {
      if (!(candidates[candidate] in actors))
        print candidate
    }
  }
' "$TEMP/demangled.txt" | LC_ALL=C sort -u >"$OUTPUT_TEMP"

if ! mv "$OUTPUT_TEMP" "$OUTPUT"; then
  echo "error: could not atomically write receiver manifest: $OUTPUT" >&2
  exit 1
fi
OUTPUT_TEMP=""
