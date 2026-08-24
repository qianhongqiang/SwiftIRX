# IRHotfixSDK

The reusable iOS hotfix engine lives in this directory.

```text
IRHotfixSDK/
├── ABI/
│   ├── HFStatus.h
│   ├── HFHandle.h
│   ├── HFValue.h
│   ├── HFPatchFrame.h
│   ├── HFDescriptor.h
│   └── IRHotfixABI.h
├── Runtime/
│   ├── Hotfix.swift
│   └── HotfixPatchAnnotation.swift
├── Format/
│   ├── HFIR.h
│   ├── HFIR.cpp
│   ├── HFPatchContainer.h
│   ├── HFPatchContainer.cpp
│   └── HFIRFormat.md
├── Bridge/
│   ├── IRHotfixObjCBridge.h
│   ├── IRHotfixObjCBridge.mm
│   └── IRHotfixValue.h
├── VM/
│   ├── HFIRRuntime.h
│   └── HFIRRuntime.cpp
├── HostAdapter/
│   ├── Generated/
│   │   ├── HFGeneratedHostAdapters.mm
│   │   └── HotfixGeneratedHostAdapters.swift
│   ├── HFGeneratedHostAdapters.h
│   ├── HFHostAdapter.h
│   ├── HFHostAdapter.hpp
│   ├── HFHostAdapterRegistry.cpp
│   ├── HotfixHostAdapterManifest.swift
│   └── HotfixHostAdapter.swift
└── Support/
    └── IRHotfix-Bridging-Header.h
```

- `ABI/` is the versioned public C boundary shared by compiler-generated
  trampolines, the VM gateway, and native host adapters. `HFPatchFrame` is
  the sole invocation envelope; its embedded `HFHandle` and `HFValue` fields
  do not expose Swift or Objective-C object layouts.
- `Runtime/Hotfix.swift` contains the binary patch lifecycle API, HFIR gateway,
  and Codable Target Manifest model.
- `Runtime/HotfixPatchAnnotation.swift` declares the build-only
  `@HotfixPatch` marker used to select changed functions on a patch branch.
- `Format/` defines typed HFIR v2, the deterministic `.hfpatch` v1 container,
  semantic validation, binary encoding/decoding, and human-readable dumping.
  It is C++20 with no LLVM dependency so the same code runs in host tools and
  the iOS VM.
- `VM/HFIRRuntime.cpp` owns the validated HFIR registry and C++20 interpreter.
  It executes typed registers, locals, control flow, calls, strings, and
  descriptor-driven host operations with instruction/call-depth limits.
- `HostAdapter/HFHostAdapter.h` defines the stable descriptor, call frame,
  registry and gateway C ABI shared by Objective-C, C, Swift and C++.
- `HostAdapter/HFHostAdapterRegistry.cpp` validates descriptors and values,
  owns adapter contexts safely across concurrent invocations, and routes every
  host call without a per-symbol switch. Objective-C is a built-in adapter;
  native languages register ABI-owning gateways.
- `HostAdapter/HFHostAdapter.hpp` provides compile-time C/C++ scalar and method
  thunks. `HotfixHostAdapter.swift` provides one shared C trampoline backed by
  Swift closures, so each Swift function does not require handwritten marshal
  code.
- `HotfixConfig/HotfixHostAdapters.json` is the app's declarative adapter
  allowlist. `HotfixAdapterTool` validates it and generates the Swift/C/C++
  registration translation units plus `HotfixHostAdapterManifest.json` before
  Sources compile. `HotfixManager` installs those registrations exactly once.
- `Bridge/IRHotfixObjCBridge.h` exposes the Objective-C invocation ABI used by VM
  cores.
- `Bridge/IRHotfixObjCBridge.mm` resolves Objective-C method signatures at
  runtime and invokes methods without a per-selector implementation table.
- `Bridge/IRHotfixValue.h` defines the bridge-local `IRHFValue` representation
  used between the generic host registry and `NSInvocation`; native adapters
  use the public `HFValue` and `HFHostCallFrame` ABI instead.
- `Support/IRHotfix-Bridging-Header.h` is the app target's Swift bridging-header
  integration point.

`HF_ABI_VERSION` is currently `3`. Version 3 replaces address-bearing receiver
values with generation-checked Host Handle Table entries. It retains the
committed-execution status semantics introduced by version 2; an older
trampoline is rejected before patch execution. Every caller
writes both `abiVersion` and `structSize`; the runtime validates them before
reading arguments. A trampoline
uses the patch result only for `HFStatusApplied`. Failures before host effects
begin preserve the original implementation fallback. After a potentially
effectful host call begins, `HFStatusExecutionCommitted` suppresses fallback;
void functions return and value-returning functions trap rather than run the
original implementation with duplicated effects.

Object receivers and host-object values are represented by opaque `HFHandle`
values backed by the Host Handle Table. A handle contains a slot ID and
generation; stale generations are rejected. Entries support strong, weak, and
borrowed ownership, and invocation leases pin a resolved object for the duration
of a host call. ObjC and Swift objects use the same registry and can safely span
multiple calls when the owning handle is retained.

Compiler output calls the status-returning `hf_vm_invoke` directly. The app has
no Boolean compatibility gateway and no runtime LLVM-text interpreter.

## Native Host Adapters

External calls in patch source lower to `host.call` imports. The lowerer uses
explicit LLVM calling-convention and `swiftself` metadata to classify them as
`native-c`, `native-swift`, or `native-cxx`. Unsupported varargs, indirect
calls, hidden ABI parameters, and noncanonical integer widths fail compilation.

For released functions, add one descriptor to
`HotfixConfig/HotfixHostAdapters.json`. Swift function and instance-method
bindings generate typed Swift marshal closures. C functions and C++ functions
or instance methods generate calls to the templated gateway in an
Objective-C++ translation unit; their declaration header must be named in the
descriptor. Xcode generates and bundles a deterministic
`HotfixHostAdapterManifest.json`, so available native imports can be archived
alongside `HotfixTargetManifest.json` without maintaining registration code by
hand.

A Swift host function can be registered once at app startup:

```swift
let call = HotfixSwiftHostCall(
    symbol: "$s2IR14nativeMultiplyyS2iF",
    returnKind: HFValueKind(HFValueKindSignedInteger),
    argumentKinds: [HFValueKind(HFValueKindSignedInteger)]
)
let registration = try HotfixSwiftHostAdapter.register(call) { _, arguments in
    HFMakeValue(HFValueKind(HFValueKindSignedInteger), arguments[0].bits &* 2)
}
```

Keep the returned registration alive while patches may call the symbol. C uses
`hf_host_adapter_register` with an `HFHostAdapterEntry`. C++ can use
`irhotfix::host::registerFunction` or `registerMethod`; templates perform typed
marshal at compile time. In all cases, the VM sees only `HFValue`, `HFHandle`
and `HFHostCallFrame`, never a native function pointer ABI.

Native imports are preflighted before the first patch instruction, so a missing
or signature-mismatched adapter cannot fail after earlier host side effects.
Registrations carry opaque generation tokens to make stale unregister calls
harmless. Keep registrations alive for every active patch that references them;
Swift handlers are `@Sendable` and may be invoked concurrently unless marked
main-thread-only.

HFIR `i64` is a canonical 64-bit bit pattern and uses
`HFValueKindSignedInteger` in host descriptors; the C++ `UInt64` codec preserves
all bits while using that same descriptor identity. C++ member receivers are
never inferred from a mangled name or first pointer argument. Compiler-authored
LLVM must declare `"irhotfix.receiver-index"="N"`; without that explicit
descriptor metadata, the call remains a free/static function call.

Non-null borrowed handles must remain valid for the complete patch invocation.
A retained handle is accepted only from an adapter registered with
`HFHostCallFlagObjCCompatibleHandles`, because the VM releases it through the
Objective-C runtime. Unmanaged C/C++ pointers must therefore use borrowed
ownership and an application-managed lifetime. Aggregate result buffers are
borrowed only long enough for the VM's immediate copy.
The VM acquires strong adapter leases during preflight and holds them through
the complete patch invocation, so unregister/re-register cannot invalidate
borrowed results or context storage while they are consumed. Native code using
the public C API must also use `hf_host_adapter_acquire` plus
`hf_host_adapter_invoke_leased` for Bytes or borrowed HostHandle results; the
one-shot `hf_host_adapter_invoke` accepts only scalar, void, null-handle, and
retained Objective-C-compatible native results.

Adapters are conservatively treated as potentially effectful. Set
`HFHostCallFlagNoSideEffects` (or `noSideEffects: true` in the Swift descriptor)
only when the call never mutates externally visible state, including on error.
Once any other native adapter or Objective-C operation begins, a later VM fault
returns `HFStatusExecutionCommitted` and cannot fall back to the original body.

## Target Manifest

Debug and Release instrumentation generate one descriptor-derived sidecar
manifest for each Swift object. After the target's Sources phase, Xcode merges those files
into `HotfixTargetManifest.json` in the app bundle. The JSON root contains
`schemaVersion`, `abiVersion`, and a deterministic symbol-sorted `targets`
array. Each target records:

- the exact mangled LLVM symbol;
- `targetID` and `signatureID` as 16-digit hexadecimal strings prefixed by
  `0x`, avoiding loss of 64-bit precision in JSON consumers;
- the return kind and ordered argument kinds;
- whether the function has a receiver.

`HotfixTargetManifest` decodes the bundle resource, while
`HotfixTargetDescriptor.targetIDValue` and `signatureIDValue` expose checked
`UInt64` views of the stored IDs. The generator validates the C ABI version,
descriptor size and flags, argument count, both FNV-1a hashes, and duplicate ID
collisions before publishing output.

App-facing demo code stays under `IR/`. Compiler instrumentation stays under
`Tools/HotfixPass` because it is a macOS host build tool rather than an iOS
runtime SDK component.

## Build a patch from a release branch

The primary authoring workflow changes the original source in place. Start a
patch branch from the exact released revision, edit the affected function, and
attach `@HotfixPatch` to that function:

```swift
@HotfixPatch
private func setupUI() {
    let view = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
    view.backgroundColor = .yellow
    self.view.addSubview(view)
}
```

Keep the released app's `HotfixTargetManifest.json` as the immutable baseline,
and archive its `HotfixHostAdapterManifest.json` beside it for native-import
auditing,
then run from the repository root:

```bash
Tools/HotfixPass/build-patches \
  --project IR.xcodeproj \
  --scheme IR \
  --baseline-manifest Released/HotfixTargetManifest.json \
  --output PatchProducts
```

The command invokes the real Xcode build so module names, imports, compilation
conditions, bridging headers, and SDK settings match the application. During
that build the macro emits a private anchor for each annotation. The compiler
wrapper resolves the anchor to the modified function's exact mangled symbol,
requires that symbol and ABI to exist in the baseline Manifest, and lowers each
selected function through an intermediate `.irpatch`. A renamed function, a newly
added function, or an ABI-changing edit fails instead of silently producing an
unusable Patch. The publishable output is `0x<targetID>.hfpatch`; `.irpatch` is
only a build/debug intermediate and is never loaded by the app. The binary artifact contains typed HFIR,
Target and Host Import descriptors, constants, and integrity metadata; it
contains no LLVM IR or Swift mangled symbol.

Install the binary payload through the SDK lifecycle API:

```swift
let data = try Data(contentsOf: patchURL)
let activation = try HotfixManager.shared.installAndActivate(binaryPatch: data)
defer { HotfixManager.shared.deactivate(activation) }
```

`hf_vm_invoke` dispatches only to the active HFIR VM. Malformed frames,
signature mismatches, verifier failures, traps before host effects, unsupported
host types, and off-main-thread Objective-C preflight preserve native fallback.

`@HotfixPatch` is intentionally available only while `build-patches` sets
`IR_HOTFIX_PATCH_BUILD`. The first version accepts non-generic, synchronous,
non-throwing top-level functions and class instance methods whose lowered ABI
is already supported by the published target descriptor. Static/class methods,
mutating value-type methods, and functions that require local Swift helper
definitions are rejected.

`Tools/HotfixPass/swift-patch-build` remains as a low-level compatibility tool.
It compiles a separate top-level `func hotfixPatch(...)`; application patch
branches should use the annotation workflow above.
