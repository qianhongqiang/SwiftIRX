# IRHotfixSDK

Reusable runtime pieces for the IR hotfix prototype live in this directory.

- `IRHotfixObjCBridge.h` exposes a stable C ABI to Swift and future VM cores.
- `IRHotfixObjCBridge.mm` resolves Objective-C method signatures at runtime and
  invokes methods without a per-selector implementation table.
- `IRHotfixValue.h` defines the adapter-neutral `IRHFValue` ABI that the future
  C, Swift, and C++ descriptor-driven adapters can share.

The interpreter remains in the app target for now. Compiler instrumentation is
kept under `Tools/HotfixPass` because it is a host build tool rather than an iOS
runtime SDK component.
