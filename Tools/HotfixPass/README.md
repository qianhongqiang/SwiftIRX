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
  -> HotfixPatchTool ABI validation and single-function extraction
  -> .irpatch text artifact
```

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
`Tools/HotfixPass/.build/HotfixPatchTool`. Both build directories are generated
and ignored by Git.

Run the compiler-side checks after building:

```bash
Tools/HotfixPass/Tests/run-pass-tests.sh
Tools/HotfixPass/Tests/verify-swift-load.sh
Tools/HotfixPass/Tests/verify-wrapper.sh
Tools/HotfixPass/Tests/verify-patch-build.sh
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
rejection, and single-function output.

## Xcode integration

The `IR` target has two intentionally different compilation paths:

| Configuration | Swift compilation |
| --- | --- |
| Debug | Xcode sets `SWIFT_EXEC=Tools/HotfixPass/swiftc-hotfix`, disables the integrated driver and batch mode, builds the host plugin, and instruments each eligible app Swift object automatically. |
| Release | Xcode uses its native Apple Swift compiler path. The wrapper, bridging-header fingerprint, and pass build phase do not instrument Release objects. |

The wrapper recursively expands frontend `@response-file` arguments before it
identifies the primary source and output. Quoting, nested files, paths containing
spaces, missing files, and cycles are covered by the wrapper test. Relative
response paths are resolved from the frontend process's working directory.

The Debug build phase hashes `CMakeLists.txt`, both C++ tool sources, the public
`SDK/IRHotfixSDK/ABI` headers, the receiver manifest generator, and the wrapper
into `HotfixInstrumentationStamp.h`. A changed tool fingerprint changes this
bridging-header input and invalidates all app Swift objects. When the hash is
unchanged, the build phase preserves the stamp timestamp so an incremental
no-op remains incremental. Response-file contents still participate through
Xcode's normal compile invocation; they are not part of this tool fingerprint.

The wrapper forwards object jobs whose primary basename is `Hotfix.swift` or
`LLVMIRInterpreter.swift` directly to Apple `swift-frontend`. This prevents the
runtime files under `SDK/IRHotfixSDK/Runtime` from instrumenting themselves.
Rename or split those files only after updating and testing the exclusion rule.

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

The runtime initially represents the synthetic `%self` argument as
`.pointer(0)`. At entry, the interpreter replaces only that designated argument
with a structured host handle registered in `LLVMHostContext`; an ordinary null
pointer remains nil. Patch execution is synchronous and the receiver is not
retained. UIKit bridging requires the main thread.

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
| Return | `i64` (Swift `Int` on this 64-bit target), `i1` (Swift `Bool`), or `void` |
| Value arguments | Zero through eight ordered `i64`/`i1` scalars |
| Receiver | None, or one `swiftself ptr` whose exact symbol is in the generated verified receiver manifest |
| Instance context | Verified class method or synchronous actor method; the receiver does not count toward the eight-scalar limit |

The pass stops collecting at the ninth scalar, prints
`too many scalar arguments`, and leaves that definition unchanged. The Swift C
bridge independently rejects a negative or greater-than-eight
`argumentCount` before reading the argument buffers.

Shapes that normally lower outside the supported envelope include:

- `String`, `Array`, tuples, floating-point values, closures, and other
  aggregate or reference-counted Swift values.
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
`HFValue`, represents an optional object receiver as an `HFHandle`, fills a
versioned `HFPatchFrame`, and passes that one frame pointer to `hf_vm_invoke`.
It returns the frame result only when the gateway returns `HFStatusApplied`;
every other status calls the native clone with the untouched Swift ABI values.

Fallback covers no active patch, a target/signature mismatch, malformed IR,
missing entry function, entry parameter mismatch, interpreter error, execution
budget failure, malformed frame metadata, and unsupported result encoding.
Malformed metadata includes an ABI-version or structure-size mismatch, a
nonzero reserved field, more than eight arguments, a missing or unexpected
argument array, unknown flags or kinds, noncanonical boolean bits, and an
invalid receiver handle. The current interpreter path maps both an absent patch
and execution failure to `HFStatusNoPatch`; the more specific public status
values are reserved for later diagnostic propagation.

The C entry point cannot prove that an arbitrary nonnull argument pointer or a
borrowed-address handle token names readable mapped memory. It trusts those
addresses after structural validation. Pass-generated calls provide valid,
synchronous storage, but an erroneous external C caller can still cause a
process crash rather than a native fallback.

Direct recursive calls inside the native clone are rewritten to call the clone,
so native recursion does not keep entering the trampoline. Patch recursion is
guarded by a thread-local set of active target IDs: re-entry into the same target
falls back, while a different target may be nested on the same thread.

Patch registry reads and writes are serialized with `NSLock`. Each operation
copies or publishes a complete value-state snapshot, and persistence uses
`UserDefaults`; this is a teaching implementation, not a lock-free hot path or
a transactional remote-patch store.

## Interpreter execution bounds

One `LLVMIRInterpreter.run` creates a single budget shared by the root entry and
every interpreted function it calls:

- At most 200,000 basic-block visits are allowed across the whole run. Entering
  a block consumes one step; individual instructions do not each consume a
  separate step. Loops and callees therefore draw from the same counter.
- At most four interpreted function frames can be active at once: the root plus
  no more than three callees. Recursion, mutual recursion, and even a finite
  five-frame call chain fail with the call-depth error.
- `insertvalue` indices are restricted to `0..<1024`, so interpreter-created
  aggregates contain at most 1,024 elements. Negative, 1,024, and larger indices
  fail before growing an aggregate.

Duplicate basic-block labels, unsafe aggregate indices, and nonfinite,
fractional, or out-of-range integer spellings throw parser/runtime errors. A
patch invoked through `hf_vm_invoke` converts those errors and the execution
budget errors into a non-applied status, so the trampoline takes its native
fallback. A direct call to `LLVMIRInterpreter.run` receives the corresponding
error.

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
  --output IR/Patches/HotfixSetupUI.irpatch
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
target symbol, and emits exactly one defined function. It rejects calls to
non-inlined helpers defined in the Patch module; keep the first-version Patch
implementation in the `hotfixPatch` body. Calls into UIKit, Objective-C, Swift
runtime functions, and intrinsics remain external and are handled or explicitly
rejected by the VM at execution time.

The output is the complete text Patch consumed by
`installAndActivate(textPatch:)`. Rebuild it whenever the matching app's
Manifest symbol or ABI changes. After overwriting a bundled `.irpatch`, rebuild
the app so Xcode copies the new resource; a downloaded Patch can be distributed
without rebuilding the app.

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
