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
│   └── LLVMIRInterpreter.swift
├── Bridge/
│   ├── IRHotfixObjCBridge.h
│   ├── IRHotfixObjCBridge.mm
│   └── IRHotfixValue.h
└── Support/
    └── IRHotfix-Bridging-Header.h
```

- `ABI/` is the versioned public C boundary shared by compiler-generated
  trampolines, the VM gateway, and future native adapters. `HFPatchFrame` is
  the sole invocation envelope; its embedded `HFHandle` and `HFValue` fields
  do not expose Swift or Objective-C object layouts.
- `Runtime/Hotfix.swift` contains the patch registry, execution entry points,
  fallback behavior, the `hf_vm_invoke(HFPatchFrame *)` implementation, and
  the Codable Target Manifest model.
- `Runtime/LLVMIRInterpreter.swift` parses and executes the supported LLVM IR
  subset and owns structured host handles for objects, selectors, and classes.
- `Bridge/IRHotfixObjCBridge.h` exposes a stable C ABI to Swift and future VM
  cores.
- `Bridge/IRHotfixObjCBridge.mm` resolves Objective-C method signatures at
  runtime and invokes methods without a per-selector implementation table.
- `Bridge/IRHotfixValue.h` defines the adapter-neutral `IRHFValue` ABI that the
  future C, Swift, and C++ descriptor-driven adapters can share.
- `Support/IRHotfix-Bridging-Header.h` is the app target's Swift bridging-header
  integration point.

`HF_ABI_VERSION` is currently `1`. Every caller writes both `abiVersion` and
`structSize`; the runtime validates them before reading arguments. A trampoline
uses the patch result only for `HFStatusApplied`. `HFStatusNoPatch` and every
validation/execution failure preserve the original implementation fallback.

For the first version, an object receiver is represented by a structured,
synchronous borrowed `HFHandle`. Its token temporarily carries the host address
behind the `HFHandleFlagBorrowedAddress` flag. Consumers must still treat the
token as opaque: a later handle table can replace that token meaning without
changing `HFPatchFrame` layout.

`ir_hotfix_invoke` remains only as a compatibility adapter for existing callers;
new compiler output calls `hf_vm_invoke` directly.

## Target Manifest

Debug instrumentation generates one descriptor-derived sidecar manifest for
each Swift object. After the target's Sources phase, Xcode merges those files
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

## Text patch input

Application code installs a patch from text and keeps only the opaque activation
token:

```swift
let text = try String(contentsOf: patchURL, encoding: .utf8)
let activation = try HotfixManager.shared.installAndActivate(textPatch: text)
defer { HotfixManager.shared.deactivate(activation) }
```

The text must currently contain exactly one defined LLVM function. That
function's name is the target symbol and its body is the interpreter entry. The
SDK derives all other metadata:

- `targetID` from the function name;
- `signatureID` from the return and parameter types;
- receiver presence from a leading `ptr` parameter;
- `patchID` from the complete text payload.

Supported patch ABI types are `i64`, `i1`, and `void`; a leading `ptr` is the
optional receiver. The demo payloads under `IR/Patches` are complete examples.

Application authors can generate that text from Swift instead of writing LLVM
IR. Define exactly one top-level `func hotfixPatch(...)`, then run the host-side
`Tools/HotfixPass/swift-patch-build` command with the built app's
`HotfixTargetManifest.json`, a unique target query, the Swift source, and the
output `.irpatch` path. The compiler selects the real mangled target from the
Manifest and validates its receiver, scalar arguments, and return ABI before
publishing the single-function Patch. `IR/PatchSources/HotfixSetupUI.swift` is
the end-to-end UIKit example.
