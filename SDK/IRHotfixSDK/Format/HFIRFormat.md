# HFIR v1 and `.hfpatch` v1

This directory defines the stable compiler/runtime boundary of IRHotfixSDK.
LLVM IR and Swift runtime calls are compiler inputs only. They are lowered into
typed semantic HFIR before a patch is published.

## HFIR v1

HFIR is a typed register IR. A function owns an ordered register type table;
its parameter registers are the prefix of that table. Every other register has
exactly one definition. Control flow is represented by numbered basic blocks,
and every block ends in `branch`, `branch.conditional`, or `return`.

The v1 value types are:

```text
void bool i64 f64 handle string bytes point size rect
```

`handle` is an opaque host handle. It never contains an Objective-C, Swift, C,
or C++ object layout. `string` is a VM value and is bridged only when a host
descriptor requests it. Geometry constants use canonical little-endian IEEE
754 doubles: point and size contain two doubles; rect contains four doubles in
`x, y, width, height` order.

The v1 instruction families are:

```text
nop const move
add/sub/mul/div .i64 and .f64
compare.eq/ne/lt/le/gt/ge
branch branch.conditional return
local.alloc local.load local.store
object.class object.construct object.invoke object.release
string.constant function.call
```

Calls do not name Swift runtime functions or raw addresses. `object.*` resolves
an indexed Host Import descriptor. Each descriptor contains a stable ID, an
operation kind, owner/name metadata, an optional Objective-C type encoding, a
receiver flag, and typed arguments/results. Future C, Swift and C++ gateways use
the same descriptor slot without adding new VM opcodes per function or type.

`hfir::verify` rejects malformed types, references, register definitions,
terminators, call signatures, imports, constants, debug locations, and entry
descriptors before execution. Unknown enum values fail closed.

## `.hfpatch` v1

All integers in the container are unsigned little-endian. A file consists of a
64-byte header, aligned section payloads, and a terminal section directory.

```text
Header
  magic                  8 bytes: HFPATCH\0
  containerVersion       u16
  hfirVersion            u16
  abiVersion             u32
  flags                  u32
  sectionCount           u32
  headerSize             u32
  reserved               u32
  fileSize               u64
  directoryOffset        u64
  payloadHash            u64 FNV-1a over bytes after the header
  reserved               u64

Section directory entry (32 bytes)
  type                    u32
  flags                   u32
  offset                  u64
  size                    u64
  elementCount            u32
  reserved                u32
```

Required sections are Metadata, Constants, Host Imports and Functions. Debug
Information and Signature are optional. Metadata persists only `patchID`,
`targetID`, `signatureID`, and the entry function index. The compiler uses the
released Manifest to validate the original symbol and then discards that
symbol; `.hfpatch` therefore contains neither LLVM text nor Swift mangled
runtime symbols.

The container decoder validates versions, reserved fields, flags, total size,
integrity hash, directory placement, section ranges/overlaps/counts, and the
complete HFIR semantic model. No unknown section or opcode is accepted.

The Signature section stores an algorithm name, key ID, and detached signature
bytes. Version 1 provides deterministic serialization and structural support;
key lookup and cryptographic verification belong to the patch distribution
policy layer and must complete before activation.

## Host inspection tool

`HotfixPackageTool` is built with the compiler tools:

```bash
Tools/HotfixPass/.build/HotfixPackageTool create-example /tmp/setup-ui.hfpatch
Tools/HotfixPass/.build/HotfixPackageTool verify /tmp/setup-ui.hfpatch
Tools/HotfixPass/.build/HotfixPackageTool dump /tmp/setup-ui.hfpatch
```

`create-example` writes a semantic `setupUI` package containing UIView,
UILabel, color, string, and Objective-C call descriptors. It is a format
fixture, not the final compiler entry point. The next lowering stage will make
the annotation build emit this container automatically.
