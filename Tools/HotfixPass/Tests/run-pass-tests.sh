#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
TARGET_MANIFEST_TOOL="$ROOT/Tools/HotfixPass/.build/HotfixManifestTool"
FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/scalars.ll"
TARGET_MANIFEST_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/target_manifest.ll"
TARGET_MANIFEST_EXPECTED="$ROOT/Tools/HotfixPass/Tests/fixtures/target_manifest.json"
TARGET_MANIFEST_INVALID_ABI="$ROOT/Tools/HotfixPass/Tests/fixtures/target_manifest_invalid_abi.ll"
TARGET_MANIFEST_CONFLICT="$ROOT/Tools/HotfixPass/Tests/fixtures/target_manifest_conflict.json"
SWIFT_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture.swift"
ACTOR_LIBRARY_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/actor_library.swift"
ACTOR_EXTENSION_FIXTURE="$ROOT/Tools/HotfixPass/Tests/fixtures/actor_extension.swift"
MANIFEST_GENERATOR="$ROOT/Tools/HotfixPass/generate-class-receiver-manifest.sh"
OPT="/opt/homebrew/opt/llvm@19/bin/opt"
FILECHECK="/opt/homebrew/opt/llvm@19/bin/FileCheck"
LLVM_DIS="/opt/homebrew/opt/llvm@19/bin/llvm-dis"
LLVM_NM="/opt/homebrew/opt/llvm@19/bin/llvm-nm"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hotfix-pass-scalars.XXXXXX")"
SCALAR_MANIFEST="$TEMP/scalar-receivers.txt"

cleanup() {
  rm -rf "$TEMP"
}
trap cleanup EXIT

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

"$TARGET_MANIFEST_TOOL" extract \
  "$TEMP/targets.json" \
  "$TARGET_MANIFEST_FIXTURE"
if ! cmp -s "$TARGET_MANIFEST_EXPECTED" "$TEMP/targets.json"; then
  echo "error: extracted target manifest differs from expected output" >&2
  diff -u "$TARGET_MANIFEST_EXPECTED" "$TEMP/targets.json" >&2 || true
  exit 1
fi
"$TARGET_MANIFEST_TOOL" merge \
  "$TEMP/merged-targets.json" \
  "$TEMP/targets.json" \
  "$TEMP/targets.json"
if ! cmp -s "$TARGET_MANIFEST_EXPECTED" "$TEMP/merged-targets.json"; then
  echo "error: target manifest merge was not deterministic or deduplicated" >&2
  exit 1
fi
if "$TARGET_MANIFEST_TOOL" extract \
  "$TEMP/invalid-abi.json" \
  "$TARGET_MANIFEST_INVALID_ABI" \
  2>"$TEMP/invalid-abi.stderr"; then
  echo "error: target manifest tool accepted an incompatible ABI" >&2
  exit 1
fi
grep -Fq 'uses an unsupported ABI version' "$TEMP/invalid-abi.stderr"
if "$TARGET_MANIFEST_TOOL" merge \
  "$TEMP/conflicting-targets.json" \
  "$TEMP/targets.json" \
  "$TARGET_MANIFEST_CONFLICT" \
  2>"$TEMP/conflicting-targets.stderr"; then
  echo "error: target manifest tool accepted a target ID collision" >&2
  exit 1
fi
grep -Fq 'target ID collision' "$TEMP/conflicting-targets.stderr"
if "$TARGET_MANIFEST_TOOL" extract \
  "$TEMP/missing-target.json" \
  "$TEMP/missing-target.bc" \
  2>"$TEMP/missing-target.stderr"; then
  echo "error: target manifest tool accepted a missing input" >&2
  exit 1
fi
grep -Fq 'cannot read LLVM module' "$TEMP/missing-target.stderr"

find_swift_symbol() {
  local demangled_fragment="$1"
  local symbols_file="${2:-$TEMP/swift.symbols.txt}"
  local symbol
  local demangled
  local match=""

  while IFS= read -r symbol; do
    demangled="$(printf '%s\n' "$symbol" | xcrun swift-demangle --compact)"
    case "$demangled" in
      "method descriptor for "* | "protocol witness for "*) continue ;;
    esac
    if [[ "$demangled" == *"$demangled_fragment"* ]]; then
      if [[ -n "$match" ]]; then
        echo "error: multiple Swift symbols matched $demangled_fragment" >&2
        return 1
      fi
      match="$symbol"
    fi
  done <"$symbols_file"

  if [[ -z "$match" ]]; then
    echo "error: no Swift symbol matched $demangled_fragment" >&2
    return 1
  fi
  printf '%s\n' "$match"
}

assert_receiver_instrumented() {
  local symbol="$1"
  local ir_file="$2"
  local name_global

  grep -Fq "define swiftcc i64 @\"$symbol\"" "$ir_file"
  grep -Fq "define private swiftcc i64 @\"$symbol.hotfix_original\"" "$ir_file"
  awk -v symbol="$symbol" '
    index($0, "define swiftcc") && index($0, symbol) { in_function = 1 }
    in_function && /call i32 @hf_host_handle_scope_begin\(ptr %.*i16 1, ptr %/ { scope_begin = 1 }
    in_function && /load %struct.HFHandle, ptr %/ { handle_load = 1 }
    in_function && /call i32 @hf_vm_invoke/ { invoked = 1 }
    in_function && /call i32 @hf_host_handle_scope_end_ref\(ptr %/ { scope_end = 1 }
    in_function && /^}/ { in_function = 0 }
    END { exit scope_begin && handle_load && invoked && scope_end ? 0 : 1 }
  ' "$ir_file"
  name_global="$(
    grep -F "c\"$symbol\\00\"" "$ir_file" |
      sed -E 's/^(@[^ ]+).*/\1/'
  )"
  if [[ -z "$name_global" ]]; then
    echo "error: receiver $symbol has no descriptor name constant" >&2
    return 1
  fi
  grep -F 'private constant %struct.HFDescriptor {' "$ir_file" |
    grep -F 'i32 1, i32 0, ptr' |
    grep -F 'section "__DATA,__hotfix"' |
    grep -Fq "ptr $name_global"
}

printf '%s\n' \
  'absentFromModule' \
  'eightScalarArguments' \
  'instanceTarget' \
  >"$SCALAR_MANIFEST"

IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$SCALAR_MANIFEST" "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/named.ll" \
  2>"$TEMP/named.stderr"
EXPECTED_DIAGNOSTICS=$'[HotfixPass] skip nineScalarArguments: too many scalar arguments\n[HotfixPass] skip unverifiedReceiver: unverified swiftself receiver\n[HotfixPass] skip unsupportedPointer: unsupported non-receiver pointer argument\n[HotfixPass] skip variadicTarget: variadic function'
if [[ "$(<"$TEMP/named.stderr")" != "$EXPECTED_DIAGNOSTICS" ]]; then
  echo "error: named pass diagnostics differ from the expected deterministic output" >&2
  diff -u <(printf '%s\n' "$EXPECTED_DIAGNOSTICS") "$TEMP/named.stderr" >&2 || true
  exit 1
fi
"$FILECHECK" \
  --check-prefixes=CHECK,NO-CLONES \
  "$FIXTURE" \
  <"$TEMP/named.ll"

IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$SCALAR_MANIFEST" "$OPT" \
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
if ! grep -Fq '@llvm.compiler.used = appending global [22 x ptr]' "$TEMP/repeated.ll"; then
  echo "error: repeated pass duplicated retained function entries" >&2
  exit 1
fi
if [[ "$(grep -Fc 'section "__DATA,__hotfix"' "$TEMP/repeated.ll")" != "11" ]]; then
  echo "error: repeated pass duplicated or omitted hotfix descriptors" >&2
  exit 1
fi
if ! grep -Fq '@llvm.used = appending global [11 x ptr]' "$TEMP/repeated.ll"; then
  echo "error: repeated pass duplicated retained descriptor entries" >&2
  exit 1
fi

IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$SCALAR_MANIFEST" "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes='default<O0>' \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/automatic.ll"
"$FILECHECK" \
  --check-prefix=AUTO \
  "$FIXTURE" \
  <"$TEMP/automatic.ll"

IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$SCALAR_MANIFEST" "$OPT" \
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

env -u IR_HOTFIX_CLASS_RECEIVER_MANIFEST "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  -S "$FIXTURE" \
  -o "$TEMP/no-manifest.ll" \
  2>"$TEMP/no-manifest.stderr"
"$FILECHECK" \
  --check-prefix=NO-MANIFEST \
  "$FIXTURE" \
  <"$TEMP/no-manifest.ll"
grep -Fq 'define private swiftcc i64 @integerTarget.hotfix_original' "$TEMP/no-manifest.ll"
if grep -Fq '@instanceTarget.hotfix_original' "$TEMP/no-manifest.ll"; then
  echo "error: a receiver was instrumented without a class manifest" >&2
  exit 1
fi
grep -Fq '[HotfixPass] skip instanceTarget: unverified swiftself receiver' "$TEMP/no-manifest.stderr"
grep -Fq '[HotfixPass] skip unverifiedReceiver: unverified swiftself receiver' "$TEMP/no-manifest.stderr"

if IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$TEMP/missing.manifest" "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -disable-output "$FIXTURE" \
  2>"$TEMP/missing-manifest.stderr"; then
  echo "error: an unreadable class receiver manifest did not fail the pass" >&2
  exit 1
fi
grep -Fq '[HotfixPass] error: cannot read class receiver manifest' "$TEMP/missing-manifest.stderr"

if ! IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$TEMP/stale.manifest" "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  -S "$TEMP/named.ll" \
  -o "$TEMP/already-instrumented.ll" \
  2>"$TEMP/already-instrumented.stderr"; then
  echo "error: stale manifest state broke an already-instrumented module" >&2
  exit 1
fi
if [[ -s "$TEMP/already-instrumented.stderr" ]]; then
  echo "error: an already-instrumented module emitted diagnostics" >&2
  exit 1
fi

xcrun swiftc \
  -O \
  -emit-bc \
  -parse-as-library \
  -module-name HotfixReceiverPassFixture \
  "$SWIFT_FIXTURE" \
  -o "$TEMP/swift.input.bc"
"$LLVM_DIS" "$TEMP/swift.input.bc" -o "$TEMP/swift.input.ll"
"$LLVM_NM" --defined-only --just-symbol-name "$TEMP/swift.input.bc" |
  sed -n 's/^_\(\$s.*\)$/\1/p' |
  LC_ALL=C sort -u >"$TEMP/swift.symbols.txt"

INSTANCE_SYMBOL="$(
  find_swift_symbol '.HotfixReceiverFixture.instanceTarget('
)"
STATIC_SYMBOL="$(
  find_swift_symbol '.HotfixReceiverFixture.staticTarget('
)"
ACTOR_ARGUMENT_SYMBOL="$(
  find_swift_symbol '.HotfixReceiverFixture.actorArgumentTarget('
)"
STRUCT_SYMBOL="$(
  find_swift_symbol '.HotfixValueFixture.mutatingTarget('
)"
ENUM_SYMBOL="$(
  find_swift_symbol '.HotfixEnumFixture.enumTarget('
)"
ACTOR_SYMBOL="$(
  find_swift_symbol '.HotfixActorFixture.actorTarget('
)"
ACTOR_EXECUTOR_SYMBOL="$(
  find_swift_symbol '.HotfixActorFixture.unownedExecutor.getter'
)"
PROTOCOL_SYMBOL="$(
  find_swift_symbol '.HotfixProtocolFixture.protocolTarget('
)"
CLASS_ARGUMENT_SYMBOL="$(
  find_swift_symbol '.classArgumentTarget('
)"
CLASS_RETURN_SYMBOL="$(
  find_swift_symbol '.classReturnTarget()'
)"

for symbol in \
  "$INSTANCE_SYMBOL" \
  "$STATIC_SYMBOL" \
  "$ACTOR_ARGUMENT_SYMBOL" \
  "$STRUCT_SYMBOL" \
  "$ACTOR_SYMBOL"; do
  grep -F "$symbol" "$TEMP/swift.input.ll" | grep -Fq 'swiftself'
done

"$MANIFEST_GENERATOR" "$TEMP/swift.input.bc" "$TEMP/swift.manifest"
printf '%s\n' \
  "$INSTANCE_SYMBOL" \
  "$ACTOR_ARGUMENT_SYMBOL" \
  "$ACTOR_SYMBOL" \
  "$ACTOR_EXECUTOR_SYMBOL" |
  LC_ALL=C sort -u >"$TEMP/swift.expected-manifest"
if ! cmp -s "$TEMP/swift.expected-manifest" "$TEMP/swift.manifest"; then
  echo "error: generated class receiver manifest contained unexpected symbols" >&2
  diff -u "$TEMP/swift.expected-manifest" "$TEMP/swift.manifest" >&2 || true
  exit 1
fi
for symbol in \
  "$STATIC_SYMBOL" \
  "$STRUCT_SYMBOL" \
  "$ENUM_SYMBOL" \
  "$PROTOCOL_SYMBOL" \
  "$CLASS_ARGUMENT_SYMBOL" \
  "$CLASS_RETURN_SYMBOL"; do
  if grep -Fxq "$symbol" "$TEMP/swift.manifest"; then
    echo "error: generated class receiver manifest admitted $symbol" >&2
    exit 1
  fi
done

if "$MANIFEST_GENERATOR" "$TEMP/missing-input.bc" "$TEMP/unused.manifest" \
  2>"$TEMP/generator-missing-input.stderr"; then
  echo "error: manifest generator accepted a missing input" >&2
  exit 1
fi
grep -Fq 'error: input bitcode is not readable' "$TEMP/generator-missing-input.stderr"
if "$MANIFEST_GENERATOR" \
  "$TEMP/swift.input.bc" \
  "$TEMP/missing-output-directory/unused.manifest" \
  2>"$TEMP/generator-output.stderr"; then
  echo "error: manifest generator accepted an invalid output path" >&2
  exit 1
fi
grep -Fq 'error: output directory is not writable' "$TEMP/generator-output.stderr"

IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$TEMP/swift.manifest" "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  "$TEMP/swift.input.bc" \
  -o "$TEMP/swift.transformed.bc" \
  2>"$TEMP/swift.stderr"
"$LLVM_DIS" "$TEMP/swift.transformed.bc" -o "$TEMP/swift.transformed.ll"
assert_receiver_instrumented "$INSTANCE_SYMBOL" "$TEMP/swift.transformed.ll"
assert_receiver_instrumented "$ACTOR_SYMBOL" "$TEMP/swift.transformed.ll"
for symbol in "$STATIC_SYMBOL" "$STRUCT_SYMBOL"; do
  grep -Fq "define swiftcc i64 @\"$symbol\"" "$TEMP/swift.transformed.ll"
  if grep -Fq "$symbol.hotfix_original" "$TEMP/swift.transformed.ll"; then
    echo "error: unsafe Swift receiver $symbol was instrumented" >&2
    exit 1
  fi
  if grep -Fq "c\"$symbol\\00\"" "$TEMP/swift.transformed.ll"; then
    echo "error: unsafe Swift receiver $symbol received a descriptor" >&2
    exit 1
  fi
  grep -Fq "[HotfixPass] skip $symbol: unverified swiftself receiver" "$TEMP/swift.stderr"
done

grep -F "$ACTOR_ARGUMENT_SYMBOL" "$TEMP/swift.transformed.ll" |
  grep -Fq 'define swiftcc'
if grep -Fq "$ACTOR_ARGUMENT_SYMBOL.hotfix_original" \
  "$TEMP/swift.transformed.ll"; then
  echo "error: unsupported Actor-argument method was instrumented" >&2
  exit 1
fi
if grep -Fq "c\"$ACTOR_ARGUMENT_SYMBOL\\00\"" \
  "$TEMP/swift.transformed.ll"; then
  echo "error: unsupported Actor-argument method received a descriptor" >&2
  exit 1
fi
grep -Fq \
  "[HotfixPass] skip $ACTOR_ARGUMENT_SYMBOL: unsupported non-receiver pointer argument" \
  "$TEMP/swift.stderr"

xcrun swiftc \
  -emit-module \
  -parse-as-library \
  -module-name ActorLib \
  "$ACTOR_LIBRARY_FIXTURE" \
  -emit-module-path "$TEMP/ActorLib.swiftmodule"
xcrun swiftc \
  -O \
  -emit-bc \
  -parse-as-library \
  -module-name ActorExtension \
  -I "$TEMP" \
  "$ACTOR_EXTENSION_FIXTURE" \
  -o "$TEMP/actor-extension.input.bc"
"$LLVM_DIS" \
  "$TEMP/actor-extension.input.bc" \
  -o "$TEMP/actor-extension.input.ll"
"$LLVM_NM" --defined-only --just-symbol-name "$TEMP/actor-extension.input.bc" |
  sed -n 's/^_\(\$s.*\)$/\1/p' |
  LC_ALL=C sort -u >"$TEMP/actor-extension.symbols.txt"
EXTERNAL_ACTOR_SYMBOL="$(
  find_swift_symbol \
    '.ExternalActor.externalActorTarget(' \
    "$TEMP/actor-extension.symbols.txt"
)"
grep -F "$EXTERNAL_ACTOR_SYMBOL" "$TEMP/actor-extension.input.ll" |
  grep -Fq 'swiftself'
"$MANIFEST_GENERATOR" \
  "$TEMP/actor-extension.input.bc" \
  "$TEMP/actor-extension.manifest"
if [[ "$(<"$TEMP/actor-extension.manifest")" != "$EXTERNAL_ACTOR_SYMBOL" ]]; then
  echo "error: generated manifest omitted the external actor extension" >&2
  exit 1
fi
IR_HOTFIX_CLASS_RECEIVER_MANIFEST="$TEMP/actor-extension.manifest" "$OPT" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -verify-each \
  "$TEMP/actor-extension.input.bc" \
  -o "$TEMP/actor-extension.transformed.bc"
"$LLVM_DIS" \
  "$TEMP/actor-extension.transformed.bc" \
  -o "$TEMP/actor-extension.transformed.ll"
assert_receiver_instrumented \
  "$EXTERNAL_ACTOR_SYMBOL" \
  "$TEMP/actor-extension.transformed.ll"
