# Swift Hotfix Pass Learning Guide

This directory contains an out-of-tree LLVM instrumentation experiment for the
`IR` iOS app. It is useful for studying Swift lowering, LLVM passes, ABI
boundaries, and synchronous fallback behavior. It is not a production hotfix
system.

## Toolchain boundary

The working pipeline deliberately crosses a file boundary:

```text
Swift source
  -> Apple Swift 6.2.4 swift-frontend -emit-bc
  -> llvm-nm + swift-demangle receiver manifest
  -> Homebrew LLVM 19.1.7 opt + libHotfixPass.dylib
  -> HotfixManifestTool descriptor extraction
  -> Homebrew LLVM 19.1.7 llc -filetype=obj
  -> per-object manifest merge
  -> Xcode links the Mach-O app or test bundle

Swift patch source + app Target Manifest
  -> Apple Swift 6.2.4 swiftc -emit-bc
  -> HotfixPatchTool exact ABI validation
  -> LLVM-to-HFIR semantic lowering
  -> descriptor-driven host.call imports for C / Swift / C++ symbols
  -> deterministic .hfpatch v1 artifact

HFIR package model
  -> IRHotfixFormat semantic verifier
  -> deterministic .hfpatch v1 container
  -> HotfixPackageTool verify/dump

Released source branch + edited @HotfixPatch function + released Target Manifest
  -> Xcode build with the application's original compiler settings
  -> IRHotfixMacrosPlugin annotation anchors
  -> HotfixPatchTool exact-symbol/ABI validation and HFIR lowering
  -> one 0x<targetID>.hfpatch per annotated function
```

Native direct calls are limited to 16 non-receiver arguments. C++ member calls
additionally require the declaration attribute
`"irhotfix.receiver-index"="N"`; the lowerer never guesses `this` from a
mangled symbol or the first pointer argument. Unsupported call shapes are
rejected while building the patch rather than deferred to runtime.

Do not pass `libHotfixPass.dylib` to Apple `swift-frontend` with
`-load-pass-plugin`. The plugin is compiled against the Homebrew/upstream LLVM
C++ ABI, while Apple's frontend does not expose the complete compatible LLVM
C++ surface needed by this pass. Direct loading fails once cloning and IR
construction APIs are used. Apple Swift emits bitcode first; only the matching
Homebrew `opt` process loads the plugin.

The wrapper requires these exact identities:

- `Apple Swift version 6.2.4 (swiftlang-6.2.4.1.4 clang-1700.6.4.2)`
- Homebrew LLVM `19.1.7`

The integration is verified with Xcode 26.3, which provides that Apple Swift
identity. The wrapper stops with an error on a compiler or LLVM mismatch. This
strict pin is intentional: LLVM bitcode, attributes, target behavior, and plugin
ABI are not assumed compatible across versions.

## Install and build

Install the host tools:

```bash
brew install llvm@19 cmake
xcrun swiftc --version
/opt/homebrew/opt/llvm@19/bin/llvm-config --version
```

From the repository root, perform a clean standalone plugin build:

```bash
rm -rf Tools/HotfixPass/.cmake-build Tools/HotfixPass/.build
cmake -S Tools/HotfixPass \
  -B Tools/HotfixPass/.cmake-build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build Tools/HotfixPass/.cmake-build --config Release
```

The plugin and host executables are written to
`Tools/HotfixPass/.build/libHotfixPass.dylib` and
`Tools/HotfixPass/.build/HotfixManifestTool` and
`Tools/HotfixPass/.build/HotfixAdapterTool` and
`Tools/HotfixPass/.build/HotfixPatchTool` and
`Tools/HotfixPass/.build/HotfixPackageTool`. Both build directories are generated
and ignored by Git.

Run the compiler-side checks after building:

```bash
Tools/HotfixPass/Tests/run-pass-tests.sh
Tools/HotfixPass/Tests/verify-swift-load.sh
Tools/HotfixPass/Tests/verify-wrapper.sh
Tools/HotfixPass/Tests/verify-patch-build.sh
Tools/HotfixPass/Tests/verify-hfpatch-format.sh
Tools/HotfixPass/Tests/verify-host-adapters.sh
```

`run-pass-tests.sh` checks scalar and receiver classification, deterministic
skip diagnostics, descriptor extraction and manifest merging, collision
rejection, fallback clones, recursion, retention, and O0/O2 pipelines.
`verify-swift-load.sh` crosses the real Apple-bitcode/Homebrew-LLVM boundary,
links two transformed modules, and exports their combined targets.
`verify-wrapper.sh` checks object and sidecar emission, native fallback
execution, response files, section/symbol presence, and default source
exclusion. `verify-patch-build.sh` compiles Swift patch sources and checks target
selection, receiver and scalar ABI validation, ambiguous queries, local helper
rejection, annotation extraction, semantic HFIR lowering, and UIKit/Objective-C
imports without leaking compiler symbols.
`verify-hfpatch-format.sh` checks HFIR semantic rejection, deterministic binary
round-tripping, optional sections, dump output, corruption detection, and the
absence of LLVM IR and Swift mangled symbols from the published container.
It also executes the C++20 VM against arithmetic and descriptor-driven
Objective-C packages.

External direct calls are published as typed Host Imports. Classification uses
LLVM calling convention, Swift's explicit `swiftself` attribute, and C++ symbol
identity; runtime invocation is performed only by a matching registered
`HFHostCallDescriptor`. The lowerer does not emit `dlsym` casts or signature
switches.

## Patch-branch extraction

Build the release normally and archive its bundled
`HotfixTargetManifest.json`. To prepare a later fix, branch from that released
revision, modify the original function body, add `@HotfixPatch`, and run:

```bash
Tools/HotfixPass/build-patches \
  --project IR.xcodeproj \
  --scheme IR \
  --baseline-manifest Released/HotfixTargetManifest.json \
  --output PatchProducts
```

`build-patches` builds the pinned SwiftSyntax macro executable, enables the
annotation only for this build, and compiles one arm64 slice using Xcode. The
wrapper removes coverage instrumentation from extracted IR, while leaving the
application's other frontend settings intact. Patch mode skips generation of a
new Target Manifest because compatibility is always checked against the
released baseline.

The generated macro peer calls the annotated original function solely to make
the compiler-emitted relationship explicit. `HotfixPatchTool
extract-annotated` follows that anchor, selects the real modified definition,
requires an exact baseline symbol match, validates the published descriptor,
and publishes the semantic `.hfpatch`. An inspectable `.irpatch` intermediate
may be emitted for build diagnostics but is not a runtime format. The lowerer rejects unknown LLVM
instructions and calls with the exact offending instruction; it never
substitutes a guessed value.

The older `swift-patch-build` command remains useful for low-level extractor
fixtures that define a separate `func hotfixPatch(...)`; it is not the primary
application authoring workflow.

## Xcode integration

The `IR` target has two intentionally different compilation paths:

| Configuration | Swift compilation |
| --- | --- |
| Debug | Xcode sets `SWIFT_EXEC=Tools/HotfixPass/swiftc-hotfix`, disables the integrated driver and batch mode, builds the host plugin, and instruments each eligible app Swift object automatically. |
| Release | Uses the same wrapper and per-file instrumentation path, then runs LLVM's optimized pipeline and `llc -O2`. The archived app therefore contains the trampolines and Target Manifest required by later patch branches. |

The wrapper recursively expands frontend `@response-file` arguments before it
identifies the primary source and output. Quoting, nested files, paths containing
spaces, missing files, and cycles are covered by the wrapper test. Relative
response paths are resolved from the frontend process's working directory.

The Debug and Release build phase hashes `CMakeLists.txt`, the pass and host-tool sources, the public
`SDK/IRHotfixSDK/ABI` headers, the receiver manifest generator, and the wrapper
into `HotfixInstrumentationStamp.h`. A changed tool fingerprint changes this
bridging-header input and invalidates all app Swift objects. When the hash is
unchanged, the build phase preserves the stamp timestamp so an incremental
no-op remains incremental. Response-file contents still participate through
Xcode's normal compile invocation; they are not part of this tool fingerprint.

The wrapper forwards object jobs whose primary basename is `Hotfix.swift`,
`HotfixHostAdapter.swift`, or `HotfixGeneratedHostAdapters.swift` directly to
Apple `swift-frontend`. This prevents the runtime and generated gateways from
instrumenting themselves.
Rename or split those files only after updating and testing the exclusion rule.

Release input is lowered with Swift SIL optimization disabled so the LLVM pass
sees the complete target set before private functions can be inlined away. The
transformed module then runs LLVM `default<O2>` and `llc -O2`. This preserves
patchability and native code optimization, but it does not recover Swift-only
SIL optimizations; that tradeoff remains until instrumentation moves into a
compiler-compatible Swift pipeline.

To run focused integration tests on an installed simulator:

```bash
xcodebuild test -project IR.xcodeproj -scheme IR \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  -only-testing:IRTests \
  -skip-testing:IRUITests
```

With the Swift Testing framework used here, XCTest-style method selectors can
quietly select zero tests. Confirm a nonzero `totalTestCount` in the generated
`.xcresult`; the target-level command above currently runs 66 unit tests.

The full command is:

```bash
xcodebuild clean test -project IR.xcodeproj -scheme IR \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro'
```

On the currently pinned simulator runtime, the full runner can finish the test
events and then hang during simulator teardown instead of returning. Bound the
command in CI or a supervising terminal (five minutes is ample for this small
project), terminate the `xcodebuild` process group after the bound, and inspect
the printed `.xcresult` with `xcrun xcresulttool` before reporting a result. A
timeout or teardown hang is not a green full suite, even if focused tests pass.

## Patch record and IDs

`HotfixPatch` is `Codable` and has exactly five persisted fields:

```json
{
  "id": "patch.app.add.v1",
  "targetID": 10241782660172724343,
  "signatureID": 11825290470482846177,
  "entryFunction": "patch",
  "ir": "define i64 @patch(i64 %value) {\nentry:\n  %result = add i64 %value, 10\n  ret i64 %result\n}"
}
```

- `id` is an application-defined patch identity.
- `targetID` is FNV-1a 64 over the UTF-8 bytes of the exact original LLVM
  function name. For the example it is
  `$s2IR13hotfixableAddyS2iF`, without Mach-O's leading underscore.
- `signatureID` is FNV-1a 64 over one canonical ASCII string:
  `return=<type>;arguments=<comma-separated scalar types>;receiver=<0-or-1>`.
- `entryFunction` names the function the interpreter executes; it need not be
  `patch`, but the IR must define the chosen name.
- `ir` contains the interpreter input.

For the JSON above the canonical signature is
`return=i64;arguments=i64;receiver=0`. Both hashes use the standard FNV-1a
offset basis `14695981039346656037` and prime `1099511628211`, with wrapping
64-bit multiplication. IDs are ABI data: recompute them whenever the mangled
symbol, scalar order, return kind, or receiver presence changes.

## Entry IR

A top-level `Int`/`Bool` patch contains only the scalar parameters:

```llvm
define i64 @patch(i64 %value, i1 %enabled) {
entry:
  %selected = select i1 %enabled, i64 %value, i64 0
  ret i64 %selected
}
```

A verified class or synchronous actor method receives a synthetic receiver
pointer first, followed by the scalar parameters in their original order:

```llvm
define i64 @patch(ptr %self, i64 %value) {
entry:
  %result = add i64 %value, 100
  ret i64 %result
}
```

The trampoline registers synthetic `%self` in the Host Handle Table and passes
only `token + generation + kind + ownership` in `HFHandle`. A borrowed entry is
released after the synchronous invocation. Raw host addresses are exposed only
inside a validated invocation lease. UIKit bridging requires the main thread.

The patch author must make the entry return kind agree with the canonical
signature return kind. The runtime checks entry parameter types, but it does not
currently cross-check an `i64` patch result against an `i1` target or vice versa.
An `i1` result used by an `i64` trampoline becomes `0` or `1`; an `i64` result
used by an `i1` trampoline is truncated by that trampoline.

## Supported ABI envelope

The pass classifies lowered LLVM ABI, not Swift source spelling.

| Part | Supported |
| --- | --- |
| Function | A defined `swiftcc` function that is not a generated hotfix symbol |
| Return | `i64` (`Int`), `i1` (`Bool`), `float` (`Float`), `double` (`Double`), or `void` |
| Value arguments | Zero through eight ordered integer, boolean, Float, or Double scalars |
| Receiver | None, or one `swiftself ptr` whose exact symbol is in the generated verified receiver manifest |
| Instance context | Verified class method or synchronous actor method; the receiver does not count toward the eight-scalar limit |

The pass stops collecting at the ninth scalar, prints
`too many scalar arguments`, and leaves that definition unchanged. The Swift C
bridge independently rejects a negative or greater-than-eight
`argumentCount` before reading the argument buffers.

Shapes that normally lower outside the supported envelope include:

- `String`, `Array`, tuples, closures, and other aggregate or
  reference-counted values as public target parameters or results.
- Any ordinary pointer argument, including class/object arguments that are not
  the one verified receiver; pointer returns are also unsupported.
- Unverified `swiftself`, multiple or malformed receivers, value-semantic
  receivers, class static metadata receivers, and lifecycle/metadata entries.
- `async`, `throws`, generic, witness-table, `inout`, indirect-result, and
  variadic functions or patches.
- More than eight scalar arguments.

Source spelling alone does not make a Swift type unsupported. With the pinned
compiler, a single-field struct or single-payload enum can lower to `i64` and be
instrumented as a scalar; the pass preserves no struct/enum semantic tag for the
patch. A definition is skipped only when its emitted LLVM ABI falls outside the
table, for example through a pointer, aggregate, unsupported return, or variadic
shape. This prototype does not promise source-level semantic recognition.

The receiver manifest is fail closed. With no manifest, scalar-only functions
can still be instrumented but every pointer `swiftself` is unverified. A
configured but unreadable manifest is a hard pass error. The generator uses
`llvm-nm` plus `swift-demangle --expand` and accepts callable class contexts;
Swift 6.2.4 demangles supported actor contexts through that class-shaped path.

## Transformation and fallback

Each eligible definition becomes a trampoline plus a private
`<symbol>.hotfix_original` clone. The trampoline marshals each scalar into an
`HFValue`, registers an optional object receiver as an `HFHandle`, fills a
versioned `HFPatchFrame`, and passes that one frame pointer to `hf_vm_invoke`.
It returns the frame result only when the gateway returns `HFStatusApplied`;
every other status calls the native clone with the untouched Swift ABI values.

Fallback covers no active patch, a target/signature mismatch, malformed HFIR,
missing entry function, entry parameter mismatch, VM error, execution
budget failure, malformed frame metadata, and unsupported result encoding.
Malformed metadata includes an ABI-version or structure-size mismatch, a
nonzero reserved field, more than eight arguments, a missing or unexpected
argument array, unknown flags or kinds, noncanonical boolean bits, and an
invalid or stale receiver handle.

The Host Handle Table rejects stale token/generation pairs, tracks borrowed,
strong, and weak entries, and creates a pinning invocation lease before a raw
address reaches Objective-C, Swift, C, or C++ adapter code. Strong result
handles transfer table-entry ownership to the VM; releasing a VM value
invalidates the entry while an already-acquired lease remains valid.

Direct recursive calls inside the native clone are rewritten to call the clone,
so native recursion does not keep entering the trampoline. HFIR execution uses
the C++ VM's instruction budget and call-depth limit; verified packages contain
no runtime-parsed LLVM text.

## Descriptor section

The pass emits one retained, 8-byte-aligned, ABI-versioned `HFDescriptor` for
every eligible function into Mach-O `__DATA,__hotfix`. Its fixed LLVM layout is:

```text
{ i32 abiVersion, i32 structSize, i64 targetID, i64 signatureID,
  i32 returnKind, i32 scalarCount, i32 flags, i32 reserved,
  ptr mangledName, ptr orderedScalarKinds }
```

The ordered kinds are 32-bit `HFValueKind` entries. Current instrumented values
use `1 = i64`, `2 = i1`, and return-only `3 = void`; descriptor flag bit zero
is set when the function has a receiver. The trampoline also embeds both IDs
directly, so invocation does not depend on scanning the section.

## Target Manifest generation

`HotfixManifestTool` reads the versioned `HFDescriptor` globals from transformed
LLVM IR or bitcode. It does not independently infer Swift signatures. Its two
commands are:

```bash
HotfixManifestTool extract output.json transformed.bc
HotfixManifestTool merge output.json first.json second.json
```

`extract` verifies the descriptor layout, value kinds, argument limit,
`targetID`, and canonical `signatureID`. `merge` revalidates every input,
deduplicates identical targets, rejects target-ID collisions, and writes stable
symbol-sorted JSON. Both IDs use `0x` plus exactly 16 lowercase hexadecimal
digits so tools that represent JSON numbers as IEEE-754 doubles cannot truncate
them.

The compiler wrapper writes `<object>.hotfix-targets.json` beside each generated
object. It never writes a shared file from concurrent frontend jobs. The Xcode
`Build Hotfix Target Manifest` phase runs after Sources and merges those
sidecars into the Debug app bundle as `HotfixTargetManifest.json`. Release
builds remain on the uninstrumented compiler path and do not publish a manifest.

## Swift Patch Compiler

Patch authors write one top-level function named `hotfixPatch`. They do not
write a mangled symbol, target ID, signature ID, LLVM calling convention, or
LLVM IR. A receiver target takes its object as the first Swift parameter;
remaining parameters and the return value must match the Target Manifest's
`i64`/`i1`/`void` ABI.

The checked-in setup UI source is
`IR/PatchSources/HotfixSetupUI.swift`. Build it against the manifest from a
Debug app build:

```bash
Tools/HotfixPass/swift-patch-build \
  --manifest /path/to/IR.app/HotfixTargetManifest.json \
  --target setupUI \
  --source IR/PatchSources/HotfixSetupUI.swift \
  --output IR/Patches/HotfixSetupUI.hfpatch
```

`--target` accepts an exact mangled symbol, hexadecimal target ID, or a unique
symbol substring. A non-unique query fails and lists every match. The builder
pins the Apple Swift identity, defaults to `arm64` iOS Simulator 26.2, injects a
private fixed compiler entry, emits unoptimized bitcode, and delegates
extraction to `HotfixPatchTool`. Use `--sdk iphoneos` for a device-targeted
compilation and override `--arch` or `--deployment-target` when they differ from
the app.

The extractor revalidates the Manifest hashes, checks the compiled LLVM return,
receiver, and ordered scalar parameters, assigns stable names to anonymous
basic blocks, lowers Swift checked integer add/subtract/multiply to the VM's
wrapping arithmetic semantics, replaces the compiler-only entry with the exact
target symbol, and extracts all directly reachable local helper functions into
the same HFIR package. Calls into UIKit, Objective-C, Swift
runtime functions, and intrinsics remain external and are handled or explicitly
rejected by the VM at execution time.

The text output is an inspectable build intermediate. `HotfixPatchTool` lowers
it to the publishable `.hfpatch`, which is the only patch format accepted by the
app runtime. Rebuild it whenever the matching app's Manifest symbol or ABI
changes.

## Supported Swift Patch language subset

Support is source-pattern driven rather than a promise to execute every LLVM
opcode. The current published subset covers:

- integer widths through an internal 64-bit representation, including
  truncate/sign-extend/zero-extend, bitwise operations, remainder, and shifts;
- Float/Double arithmetic, comparisons, and integer conversions;
- `if`, `switch`, `select`, `phi`, loops, and the VM instruction budget;
- scalar Optional/enum discriminators when Swift lowers them to integer
  branches, plus scalar fields of local value-type storage;
- constant and dynamic `CGRect` construction for Objective-C calls;
- directly reachable, non-generic local helpers;
- string literals and direct Swift string concatenation recognized by the
  pinned toolchain.

Public target parameters/results remain limited to the ABI table above.
Closures, async code, throwing functions, protocol witness calls, resilient
layout-dependent aggregates, and generic specialization are intentionally
outside this version. Unsupported lowering fails patch construction with the
offending LLVM instruction; it never supplies a guessed default value.

## Debug compromises and production limits

For transformed Debug objects the wrapper removes `-stack-check`, adds
`-no-stack-check`, and forces `-gnone`. Homebrew LLVM 19.1.7 cannot consume the
Apple Swift debug metadata in this path, and Homebrew `llc` rejects Apple's
`"probe-stack"="__chkstk_darwin"` attribute. Expect reduced source debugging
and no stack probes in those transformed objects. The excluded runtime objects
and all Release objects retain normal Apple compilation settings.

Loading and interpreting new executable logic can conflict with App Store
Review rules, platform security expectations, organizational release controls,
and incident-response requirements. This prototype has no patch authenticity
signature, trust chain, entitlement model, rollout controls, audit log,
revocation protocol, or hardened sandbox. The basic-block, call-frame, and
aggregate limits above are narrow resource guards, not a complete sandbox: for
example, there is no overall IR input-size, heap, or wall-clock budget. Persisted
JSON in `UserDefaults` is not a secure distribution channel. Do not describe or
deploy this repository as a production-ready over-the-air hotfix mechanism. A
production design would need legal and App Review review, cryptographic signing
and verification, strict authorization, replay prevention, monitoring, staged
rollout, rollback, and a substantially stronger execution sandbox.
