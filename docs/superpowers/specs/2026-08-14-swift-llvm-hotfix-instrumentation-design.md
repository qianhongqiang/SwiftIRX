# Swift LLVM Hotfix Instrumentation Design

## Goal

Instrument every eligible Swift function owned by the `IR` app target during compilation. Each instrumented function must retain its original Swift ABI, consult a runtime patch registry, execute an active LLVM IR patch when possible, and otherwise run its original implementation.

This project is a compiler and runtime learning exercise. App Store policy and remote-code review constraints are outside the scope of the first version.

## Scope

The first version instruments eligible functions by default without source annotations.

Supported function shapes:

- Top-level functions.
- Static functions.
- Class instance methods whose receiver is lowered as a `swiftself ptr` argument.
- Synchronous, non-generic functions.
- `i64`, `i1`, and `void` LLVM ABI returns.
- `i64` and `i1` LLVM ABI value arguments.

The initial implementation skips:

- Functions belonging to the hotfix runtime or LLVM IR interpreter.
- Swift runtime functions, declarations, compiler-generated thunks, metadata accessors, and value witnesses.
- Struct instance methods and other value-semantic receivers.
- Generic functions and functions with generic metadata or witness-table parameters.
- `async`, `throws`, `inout`, variadic, indirect-result, aggregate, floating-point, or closure ABI shapes.
- Initializers and deinitializers unless their emitted ABI is proven safe in a later version.

Every skipped definition must have an explicit reason available from the pass diagnostic output. Unsupported functions continue to compile and execute without a patch trampoline.

## Build Architecture

The installed Xcode 26.3 Swift 6.2.4 driver supports `-load-pass-plugin=<path>`. The first version therefore loads an LLVM New Pass Manager plugin directly through `OTHER_SWIFT_FLAGS` instead of replacing the full Swift driver.

The plugin is built out of band with CMake against LLVM headers pinned to the LLVM major version embedded in the selected Xcode toolchain. The plugin build records the expected Swift frontend and LLVM plugin API versions. Xcode integration validates those versions before adding the load flag and fails with a clear build diagnostic on a mismatch instead of loading an ABI-incompatible C++ plugin.

The compilation flow is:

```text
Swift source
  -> Swift AST and SIL
  -> LLVM IR
  -> HotfixInstrumentationPass
  -> normal LLVM optimization and object emission
  -> Mach-O link
```

The plugin registers at the beginning of the LLVM optimization pipeline so it sees functions before normal inlining and dead-code elimination. The trampoline is marked `noinline`; the private original clone remains eligible for normal optimization.

Only the app target loads the pass. Runtime and interpreter source files are excluded through a pass ignore list. A later `swiftc` wrapper may centralize plugin discovery, Xcode version checks, and compiler arguments, but it is not required for the first end-to-end implementation.

## LLVM Transformation

For each eligible definition, the pass:

1. Computes a 64-bit target ID from the original mangled symbol.
2. Computes a 64-bit signature ID from the supported LLVM return type, ordered argument types, and receiver presence.
3. Clones the original body to a private `<symbol>.hotfix_original` function.
4. Preserves the original symbol, linkage, calling convention, parameter attributes, return attributes, and `swiftself` placement on the trampoline.
5. Replaces the original body with a call to the C ABI runtime bridge.
6. Returns the patch result when the bridge reports success.
7. Calls `<symbol>.hotfix_original` with the untouched incoming values when the bridge reports failure.
8. Remaps direct recursive references inside the clone to `<symbol>.hotfix_original`, so native fallback recursion does not repeatedly traverse the trampoline.

Conceptually:

```llvm
define swiftcc i64 @calculate(i64 %value) noinline {
entry:
  ; Marshal %value and call @ir_hotfix_invoke.
  ; Branch to patched or original based on the returned i1.
}

define private swiftcc i64 @calculate.hotfix_original(i64 %value) {
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

- `argumentKinds` describes each slot as integer, boolean, or object receiver.
- `argumentBits` contains fixed-width raw values.
- `receiver` is null for top-level and static functions and contains the unretained class receiver for instance methods.
- `resultBits` is caller-provided storage for a scalar result. Void functions do not read it.

The bridge performs only synchronous work. A `false` return tells the trampoline to execute the original clone. The ABI contains no Swift-owned values, closures, errors, or reference-counted containers.

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

Patch IR uses the configured entry function, initially `patch`. Its parameter and result types match the supported target signature. A class instance method receives a synthetic `ptr` parameter before its scalar parameters.

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

## Runtime State and Failure Handling

The patch registry stores patches by patch ID and active patch ID by target ID. Reads use a thread-safe immutable snapshot so the hot path does not hold a lock while interpreting a patch. Updates publish a new snapshot after persistence succeeds.

Before execution, the runtime validates:

- An active patch exists for the target ID.
- The patch signature ID equals the trampoline signature ID.
- The declared entry function exists.
- Argument and return types match the entry function.

The bridge returns `false` for a missing patch, signature mismatch, parse error, runtime error, step-limit failure, or result mismatch. The native original implementation then runs synchronously. Failures are recorded through a diagnostic hook without crashing the target function.

A thread-local set of active target IDs prevents a patch from recursively re-entering the same target. Nested patches for different target IDs remain allowed.

At startup, descriptor discovery checks for duplicate target IDs with different mangled symbols. Conflicting targets are disabled and reported instead of allowing a patch to select an ambiguous function.

## Function Metadata

The pass emits one fixed-layout descriptor per eligible function into a Mach-O `__DATA,__hotfix` section. The descriptor contains:

- Target ID.
- Signature ID.
- Mangled symbol pointer or offset.
- Supported return kind.
- Argument count and argument-kind sequence.
- Receiver-presence flag.

A companion inspection command can extract these records from a linked binary and export JSON. Runtime patch lookup does not depend on this export because the trampoline embeds both IDs directly.

## Project Organization

Planned responsibilities:

- `Tools/HotfixPass/`: C++ pass source, build configuration, pass fixtures, and IR transformation tests.
- `IR/Hotfix.swift`: patch persistence, activation, thread-safe snapshots, invocation service, and the C bridge.
- `IR/LLVMIRInterpreter.swift`: general typed entry execution and host receiver handling.
- `IRTests/IRTests.swift`: interpreter and runtime behavior tests.
- Xcode project settings: build the plugin before Swift compilation and load it only in the intended configuration.

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
- Integer, boolean, void, and class receiver shapes are marshalled correctly.
- Unsupported ABI shapes are unchanged and report a reason.
- Runtime declarations and generated clones are not reinstrumented.
- Descriptor records are emitted.

The final integration test builds a Swift fixture with the loaded plugin, inspects the emitted IR, and runs an app-side function before and after activating a patch.

## Success Criteria

The first version is complete when:

- A normal supported Swift function is instrumented without a source annotation.
- Its unpatched result is identical to the original implementation.
- Activating a matching LLVM IR patch changes its result.
- Deactivating or breaking the patch restores the original result without rebuilding the app.
- A supported class instance method can execute a patch with its live receiver in the host context.
- Unsupported functions still compile, remain behaviorally unchanged, and have an observable skip reason.
- Unit, pass, and integration tests pass under the pinned Xcode toolchain.
