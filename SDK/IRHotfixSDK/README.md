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
