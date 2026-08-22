# IRHotfixSDK

The reusable iOS hotfix engine lives in this directory.

```text
IRHotfixSDK/
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

- `Runtime/Hotfix.swift` contains the patch registry, execution entry points,
  fallback behavior, and the C-callable ABI used by instrumented code.
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
