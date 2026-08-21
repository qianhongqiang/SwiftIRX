# Swift LLVM Hotfix Instrumentation Design

## Goal

Instrument every eligible Swift function owned by the `IR` app target during compilation. Each instrumented function must retain its original Swift ABI, consult a runtime patch registry, execute an active LLVM IR patch when possible, and otherwise run its original implementation.

This project is a compiler and runtime learning exercise. App Store policy and remote-code review constraints are outside the scope of the first version.

## Scope

The first version instruments eligible functions by default without source annotations.

Supported function shapes:

- Top-level functions.
- Static functions that do not carry a pointer `swiftself` parameter.
- Class instance methods whose receiver is lowered as `swiftself ptr` and whose
  exact LLVM symbol appears in the generated class receiver manifest.
- Synchronous actor instance methods meeting the same manifest requirement.
- Synchronous, non-generic functions.
- `i64`, `i1`, and `void` LLVM ABI returns.
- Up to eight `i64` and `i1` LLVM ABI value arguments; a verified receiver does
  not count toward this limit.

The initial implementation skips:

- Functions belonging to the hotfix runtime or LLVM IR interpreter.
- Swift runtime functions, declarations, compiler-generated thunks, metadata accessors, and value witnesses.
- Struct instance methods and other value-semantic receivers.
- Class static methods and protocol, struct, or enum methods whose pointer
  `swiftself` does not represent a supported object receiver.
- Generic functions and functions with generic metadata or witness-table parameters.
- `async`, `throws`, `inout`, variadic, indirect-result, aggregate, floating-point, or closure ABI shapes.
- Initializers and deinitializers unless their emitted ABI is proven safe in a later version.

Definitions that enter ABI classification and are rejected have an explicit
reason in the pass diagnostic output. Declarations, non-Swift calling
conventions, intrinsics, and generated hotfix symbols are non-candidates and do
not emit skip diagnostics. Unsupported functions continue to compile and
execute without a patch trampoline.

## Build Architecture

The pass is built against Homebrew LLVM 19.1.7 and runs inside that release's `opt`. It is intentionally not loaded into Apple `swift-frontend`: the frontend does not export the complete LLVM C++ ABI required by a transformation pass, and direct loading was verified to fail once the pass used cloning and IR construction APIs.

Apple Swift 6.2.4 emits LLVM bitcode to a file boundary. A `swiftc` wrapper first runs `generate-class-receiver-manifest.sh`, then invokes Homebrew `opt` with the named `hotfix-instrument` pipeline and `IR_HOTFIX_CLASS_RECEIVER_MANIFEST` pointing at that manifest before continuing object emission with pinned Homebrew `llc`. Integration validates the exact Swift and Homebrew LLVM versions before crossing that boundary. Xcode 26.3 must use the external Swift driver (`SWIFT_USE_INTEGRATED_DRIVER=NO`) because its integrated driver rejects a custom `SWIFT_EXEC` basename; batch mode is disabled so each frontend object job has one primary source and output. Frontend response files are recursively expanded before classification using LLVM GNU tokenization. Relative top-level and nested response paths resolve from the process working directory, matching Swift frontend behavior; cycles and unreadable files fail with wrapper diagnostics.

LLVM's `swiftself ptr` attribute is not enough to identify an object receiver.
Swift 6.2.4 also uses it for class static metadata and mutable value storage, so
casting every such pointer to `AnyObject` is unsafe. The out-of-tree prototype
therefore generates the semantic allowlist that an in-tree Swift/SIL compiler
integration would emit directly. The generator uses pinned Homebrew `llvm-nm`
to stream defined Swift symbols into `xcrun swift-demangle --expand`, accepts
only callable symbols whose first nominal context is a class, rejects static
wrappers, lifecycle entries, protocols, structs, and enums, then sorts,
deduplicates, and atomically writes exact LLVM symbols. Swift demangles actor
contexts as classes, including extensions of actors defined in other modules.
Those synchronous instance methods are intentionally accepted: an actor is a
heap object valid as an `AnyObject` receiver. The first-nominal-context rule
still prevents class types in callable arguments or returns from classifying a
top-level or value-context function as an object receiver.

If `IR_HOTFIX_CLASS_RECEIVER_MANIFEST` is unset, the receiver allowlist is
empty and pointer `swiftself` functions fail closed with `unverified swiftself
receiver`; scalar functions without receivers remain eligible. An unreadable
configured manifest is a hard pass error for an uninstrumented module.

The compilation flow is:

```text
Swift source
  -> Swift AST and SIL
  -> Apple Swift LLVM bitcode
  -> llvm-nm + swift-demangle class receiver manifest
  -> Homebrew opt + HotfixInstrumentationPass
  -> transformed LLVM bitcode
  -> object emission
  -> Mach-O link
```

The named pipeline gives the wrapper an explicit transformation boundary. The pass also registers at pipeline start for ordinary Homebrew `opt` pipelines. Both the trampoline and private original clone are marked `noinline` so fallback symbols survive subsequent optimization.

Only app-target Debug bitcode is transformed. The wrapper forwards runtime and interpreter source object jobs unchanged based on the primary source basename. It centralizes plugin discovery, toolchain checks, and compiler arguments. Because Homebrew LLVM 19.1.7 cannot parse Apple Swift debug metadata, transformed Debug objects use `-gnone`. It also removes Xcode's `-stack-check` frontend option and uses `-no-stack-check` for those objects because Homebrew `llc` rejects Apple's `"probe-stack"="__chkstk_darwin"` attribute. Excluded objects and Release retain normal Apple compilation. The CMake build phase explicitly targets the macOS host and sanitizes the inherited iOS deployment environment so the resulting pass dylib is loadable by host `opt`. It fingerprints the pass source, CMake configuration, manifest generator, and wrapper into a derived Debug-only bridging header. Xcode therefore treats instrumentation changes as a global Swift input change and recompiles app objects, while unchanged fingerprints preserve the header timestamp and retain incremental no-op behavior.

## LLVM Transformation

For each eligible definition, the pass:

1. Requires exact manifest membership before treating a pointer `swiftself` as
   a class receiver.
2. Computes a 64-bit target ID from the original mangled symbol.
3. Computes a 64-bit signature ID from the supported LLVM return type, ordered argument types, and receiver presence.
4. Clones the original body to a private `<symbol>.hotfix_original` function.
5. Preserves the original symbol, linkage, calling convention, ABI-significant attributes, and `swiftself` placement on the trampoline while removing old-body semantic promises invalidated by patch dispatch.
6. Replaces the original body with a call to the C ABI runtime bridge.
7. Returns the patch result when the bridge reports success.
8. Calls `<symbol>.hotfix_original` with the untouched incoming values when the bridge reports failure.
9. Remaps direct recursive references inside the clone to `<symbol>.hotfix_original`, so native fallback recursion does not repeatedly traverse the trampoline.
10. Marks both definitions `noinline` and records them in `llvm.compiler.used`, retaining patch points and native fallbacks through O0/O2 without changing original linkage.

Conceptually:

```llvm
define swiftcc i64 @calculate(i64 %value) noinline {
entry:
  ; Marshal %value and call @ir_hotfix_invoke.
  ; Branch to patched or original based on the returned i1.
}

define private swiftcc i64 @calculate.hotfix_original(i64 %value) noinline {
entry:
  %result = mul i64 %value, 2
  ret i64 %result
}
```

All original call sites continue to reference the original symbol and therefore pass through the trampoline. The pass must never instrument its own generated clones or runtime bridge declarations.

## Runtime ABI

The pass and Swift runtime communicate through one stable C ABI entry point exposed with `@_cdecl`:

```text
ir_hotfix_invoke(
    targetID,
    signatureID,
    argumentKinds,
    argumentBits,
    argumentCount,
    receiver,
    resultBits
) -> Bool
```

Arguments use parallel buffers:

- `argumentKinds` describes each scalar slot as integer or boolean.
- `argumentBits` contains fixed-width raw values.
- `receiver` is null for top-level and static functions and contains the unretained class or actor receiver for instance methods.
- `resultBits` is caller-provided storage for a scalar result. Void functions do not read it.

The bridge performs only synchronous work. An actor trampoline runs inside the
original actor method body after Swift has established its executor and
isolation context; an actor patch must not suspend. Async target functions and
async patches remain unsupported. A `false` return tells the trampoline to
execute the original clone. The ABI contains no Swift-owned values, closures,
errors, or reference-counted containers.

## Patch Model

`HotfixPatch` becomes:

```swift
struct HotfixPatch: Codable, Equatable {
    let id: String
    let targetID: UInt64
    let signatureID: UInt64
    let entryFunction: String
    let ir: String
}
```

Patch IR uses the configured entry function, initially `patch`. Its parameter
and result types match the supported target signature. A class or synchronous
actor instance method receives a synthetic `ptr` parameter before its scalar
parameters.

Examples:

```llvm
define i64 @patch(i64 %value, i1 %enabled) {
entry:
  ret i64 42
}
```

```llvm
define i64 @patch(ptr %self, i64 %value) {
entry:
  ret i64 42
}
```

For an instance patch, the interpreter receives `.pointer(0)` for `%self` and an `LLVMHostContext` containing the live receiver. This preserves the interpreter's existing convention in which pointer zero resolves to the host object for bridged Objective-C calls.

## Interpreter Changes

`LLVMIRInterpreter` gains a general execution API that accepts:

- An entry function name.
- Typed scalar and pointer arguments.
- An optional host context.
- A typed result supporting integer, boolean, and void.

`runMain` remains as a compatibility wrapper over the general API. Parser and runtime errors continue to use `LLVMIRInterpreterError` and include the target function name where relevant.

Each `run` has one execution budget shared by its root and nested interpreted
calls. The budget permits 200,000 basic-block visits and at most four active
interpreted frames (the root plus three callees). `insertvalue` accepts only
indices in `0..<1024`, limiting interpreter-created aggregates to 1,024
elements. Duplicate block labels, unsafe aggregate indices, and nonfinite,
fractional, or out-of-range integer spellings throw instead of continuing.

## Runtime State and Failure Handling

The patch registry stores patches by patch ID and active patch ID by target ID.
Reads and updates take an `NSLock`, copy a complete value-state snapshot, and
release the lock before interpreting a patch. Updates publish the new in-memory
snapshot after encoding and writing it to `UserDefaults`.

Before execution, the runtime validates:

- An active patch exists for the target ID.
- The patch signature ID equals the trampoline signature ID.
- The declared entry function exists.
- Supplied argument types match the entry function parameters.

The bridge returns `false` for a missing patch, signature mismatch, parse error,
runtime error, execution-budget failure, argument mismatch, unsupported result,
or a void-versus-scalar result mismatch. The native original implementation then
runs synchronously. The current implementation does not cross-check an `i64`
patch result with an `i1` target return kind or vice versa: the bridge encodes the
actual patch value, after which an `i1` trampoline truncates raw bits and an
`i64` trampoline observes a boolean as zero or one. Patch authors must make the
IR return kind agree with the return kind used to compute the canonical
signature. Interpreter failures are currently suppressed with `try?`; the
prototype does not expose a runtime diagnostic hook.

Bridge validation covers metadata such as required argument/result pointer
presence, argument count, kind values, and canonical boolean bits. It cannot
validate that an arbitrary nonnull C pointer is mapped readable or writable
memory; such addresses remain caller-trusted and an invalid external caller can
crash the process before fallback.

A thread-local set of active target IDs prevents a patch from recursively re-entering the same target. Nested patches for different target IDs remain allowed.

The current runtime does not scan descriptors at startup or detect duplicate
target IDs. The trampoline embeds both IDs and invokes the registry directly.

## Function Metadata

The pass emits one fixed-layout descriptor per eligible function into a Mach-O `__DATA,__hotfix` section. The descriptor contains:

- Target ID.
- Signature ID.
- Mangled symbol pointer or offset.
- Supported return kind.
- Argument count and argument-kind sequence.
- Receiver-presence flag.

The test suite inspects these records with Homebrew `llvm-objdump` and LLVM IR
checks. The repository does not currently ship a descriptor-to-JSON exporter.
Runtime patch lookup does not depend on descriptor extraction because the
trampoline embeds both IDs directly.

## Project Organization

Planned responsibilities:

- `Tools/HotfixPass/`: C++ pass source, build configuration, pass fixtures, and IR transformation tests.
- `IR/Hotfix.swift`: patch persistence, activation, thread-safe snapshots, invocation service, and the C bridge.
- `IR/LLVMIRInterpreter.swift`: general typed entry execution and host receiver handling.
- `IRTests/IRTests.swift`: interpreter and runtime behavior tests.
- Xcode project settings and wrapper: build the plugin and transform app bitcode only in the intended configuration.

The first integration enables the pass in Debug. Release remains unchanged until the fixture and app test suites establish toolchain compatibility.

## Testing Strategy

Implementation follows test-driven development.

Interpreter tests cover:

- Invoking a named function with integer and boolean arguments.
- Integer, boolean, and void results.
- Parameter-count and parameter-type mismatches.
- Instance receiver resolution through the host context.

Runtime tests cover:

- Missing patch falls back.
- Matching active patch executes.
- Signature mismatch falls back.
- Invalid IR and interpreter failure fall back.
- Patch results are written to the C result buffer.
- Same-target recursion falls back while different-target nesting is permitted.
- Concurrent reads and patch activation do not expose partial state.

Pass tests compile or transform small LLVM IR fixtures and assert:

- Eligible functions gain a trampoline and original clone.
- Calling convention and parameter attributes are preserved.
- Integer, boolean, void, and manifest-verified class receiver shapes are marshalled correctly.
- Local actor methods and cross-module actor extensions receive verified
  receiver trampolines, while class static, mutating value, protocol, and
  nominal argument/return decoys are excluded by real demangle output.
- Unsupported ABI shapes are unchanged and report a reason.
- Runtime declarations and generated clones are not reinstrumented.
- Descriptor records are emitted.

The final integration test emits Swift bitcode, transforms it with Homebrew `opt`, inspects the emitted IR, and runs an app-side function before and after activating a patch.

## Success Criteria

The first version is complete when:

- A normal supported Swift function is instrumented without a source annotation.
- Its unpatched result is identical to the original implementation.
- Activating a matching LLVM IR patch changes its result.
- Deactivating or breaking the patch restores the original result without rebuilding the app.
- A supported class instance method can execute a patch with its live receiver in the host context.
- Unsupported functions still compile, remain behaviorally unchanged, and have an observable skip reason.
- Unit, pass, and integration tests pass under the pinned Xcode toolchain.
