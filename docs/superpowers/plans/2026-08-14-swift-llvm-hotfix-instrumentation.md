# Swift LLVM Hotfix Instrumentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automatically instrument eligible Swift app functions with an LLVM pass so an active interpreted LLVM IR patch can replace the result while every failure falls back to the original native body.

**Architecture:** A loadable LLVM New Pass Manager plugin clones each supported `swiftcc` function and replaces its original body with a C ABI runtime dispatch trampoline. The Swift runtime validates the target/signature pair, marshals scalar arguments into the existing interpreter, and returns a typed result or tells the trampoline to call the native clone.

**Tech Stack:** Swift 6.2, Swift Testing, C++20, Swift LLVM 19.1.5 New Pass Manager, CMake, Xcode 26.3, Mach-O.

---

## File Map

- Create `Tools/HotfixPass/CMakeLists.txt`: build the version-pinned loadable pass plugin.
- Create `Tools/HotfixPass/Sources/HotfixPass.cpp`: eligibility, cloning, trampoline generation, diagnostics, and plugin registration.
- Create `Tools/HotfixPass/Tests/fixtures/scalars.ll`: deterministic LLVM fixture for integer, boolean, void, and skip behavior.
- Create `Tools/HotfixPass/Tests/fixtures/swift_fixture.swift`: actual Swift frontend compatibility fixture.
- Create `Tools/HotfixPass/Tests/run-pass-tests.sh`: build/load/transform assertions.
- Create `Tools/HotfixPass/Tests/verify-swift-load.sh`: Xcode Swift frontend smoke test.
- Create `.gitignore`: exclude CMake, plugin, object, and emitted-IR artifacts.
- Modify `IR/LLVMIRInterpreter.swift`: typed named-entry execution API.
- Modify `IR/Hotfix.swift`: target/signature patch model, thread-safe registry, runtime invocation, recursion guard, and C bridge.
- Create `IR/HotfixDemo.swift`: small supported functions used by app integration and emitted-IR inspection.
- Modify `IR/ViewController.swift`: activate/deactivate an actual function patch instead of manually running fallback IR.
- Modify `IRTests/IRTests.swift`: interpreter, registry, runtime, bridge, and integration tests.
- Modify `IR.xcodeproj/project.pbxproj`: Debug-only plugin build phase and `-load-pass-plugin` flag.
- Create `Tools/HotfixPass/README.md`: pinned toolchain, build, test, patch ABI, and skip rules.

## Task 1: Prove LLVM Plugin Compatibility

> **Compatibility correction (2026-08-14):** Although Xcode 26.3's frontend
> reports `LLVM version 17.0.0`, Apple Swift 6.2.4 is built from the
> `swiftlang/llvm-project` `swift-6.2.4-RELEASE` tag, whose pinned commit
> `8f0d2ca924db37c8f8161d55c21b9097b05b72f3` declares LLVM 19.1.5. A plugin
> compiled against Homebrew LLVM 17 loads but crashes in
> `llvm::GlobalVariable::GlobalVariable` because the C++ IR layouts differ.
> `Tools/HotfixPass/CMakeLists.txt` is authoritative: it sparsely bootstraps
> that exact Swift LLVM header revision, generates its tables with Homebrew
> LLVM 19, and rejects any mismatched frontend, source revision, or package.

**Files:**
- Create: `.gitignore`
- Create: `Tools/HotfixPass/CMakeLists.txt`
- Create: `Tools/HotfixPass/Sources/HotfixPass.cpp`
- Create: `Tools/HotfixPass/Tests/fixtures/swift_fixture.swift`
- Create: `Tools/HotfixPass/Tests/verify-swift-load.sh`

- [ ] **Step 1: Install the compatible LLVM 19 development package**

Run:

```bash
brew install llvm@19 cmake
```

Expected: `/opt/homebrew/opt/llvm@19/bin/llvm-config --version` prints `19.1.7`.

- [ ] **Step 2: Exclude native build products before building the probe**

Create `.gitignore`:

```gitignore
DerivedData/
Tools/HotfixPass/.build/
Tools/HotfixPass/.cmake-build/
Tools/HotfixPass/.toolchain/
*.bc
*.ll.tmp
```

- [ ] **Step 3: Write the failing frontend-load smoke test**

Create `Tools/HotfixPass/Tests/fixtures/swift_fixture.swift`:

```swift
func hotfixFixture(_ value: Int) -> Int {
    value * 2
}
```

Create `Tools/HotfixPass/Tests/verify-swift-load.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PLUGIN="$ROOT/Tools/HotfixPass/.build/libHotfixPass.dylib"
OUTPUT="$(mktemp -t hotfix-pass).ll"
trap 'rm -f "$OUTPUT"' EXIT

test -f "$PLUGIN"
xcrun swiftc --version | grep -q 'Apple Swift version 6.2.4'
/opt/homebrew/opt/llvm@19/bin/llvm-config --version | grep -qx '19.1.7'
xcrun swiftc -O -emit-ir \
  -parse-as-library \
  -load-pass-plugin="$PLUGIN" \
  "$ROOT/Tools/HotfixPass/Tests/fixtures/swift_fixture.swift" \
  -o "$OUTPUT"
grep -q 'hotfix-pass-loaded' "$OUTPUT"
```

Run:

```bash
chmod +x Tools/HotfixPass/Tests/verify-swift-load.sh
Tools/HotfixPass/Tests/verify-swift-load.sh
```

Expected: FAIL because the plugin does not exist.

- [ ] **Step 4: Implement the minimum loadable pass**

Create `Tools/HotfixPass/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(HotfixPass LANGUAGES C CXX)

find_package(LLVM 19.1.7 EXACT REQUIRED CONFIG PATHS /opt/homebrew/opt/llvm@19/lib/cmake/llvm NO_DEFAULT_PATH)

add_library(HotfixPass MODULE Sources/HotfixPass.cpp)
target_compile_features(HotfixPass PRIVATE cxx_std_20)
target_include_directories(HotfixPass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(HotfixPass PRIVATE ${LLVM_DEFINITIONS})
set_target_properties(HotfixPass PROPERTIES
  PREFIX "lib"
  SUFFIX ".dylib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.build"
)
target_link_options(HotfixPass PRIVATE "LINKER:-undefined,dynamic_lookup")
```

Create `Tools/HotfixPass/Sources/HotfixPass.cpp`:

```cpp
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {
class HotfixPass : public PassInfoMixin<HotfixPass> {
public:
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    auto &context = module.getContext();
    auto *bytes = ConstantDataArray::getString(context, "hotfix-pass-loaded");
    new GlobalVariable(module, bytes->getType(), true,
                       GlobalValue::ExternalLinkage, bytes,
                       "hotfix-pass-loaded");
    return PreservedAnalyses::none();
  }
};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "HotfixPass", "0.1.0",
          [](PassBuilder &builder) {
            builder.registerPipelineStartEPCallback(
                [](ModulePassManager &manager, OptimizationLevel) {
                  manager.addPass(HotfixPass());
                });
          }};
}
```

- [ ] **Step 5: Build and verify the plugin loads in both LLVM and Swift**

Run:

```bash
cmake -S Tools/HotfixPass -B Tools/HotfixPass/.cmake-build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build Tools/HotfixPass/.cmake-build
Tools/HotfixPass/Tests/verify-swift-load.sh
```

Expected: PASS and the emitted Swift IR contains `hotfix-pass-loaded`.

- [ ] **Step 6: Commit the compatibility probe**

```bash
git add .gitignore Tools/HotfixPass
git commit -m "build: prove Swift LLVM pass loading"
```

## Task 2: Add Typed Named-Function Interpretation

**Files:**
- Modify: `IR/LLVMIRInterpreter.swift:31-46`
- Modify: `IR/LLVMIRInterpreter.swift:910-924`
- Test: `IRTests/IRTests.swift`

- [ ] **Step 1: Write failing named-entry tests**

Add to `IRTests/IRTests.swift`:

```swift
@Test func interpretNamedFunctionWithTypedArguments() throws {
    let ir = """
    define i64 @patch(i64 %value, i1 %enabled) {
    entry:
      br i1 %enabled, label %yes, label %no
    yes:
      %answer = add i64 %value, 2
      ret i64 %answer
    no:
      ret i64 %value
    }
    """

    let result = try LLVMIRInterpreter().run(
        ir: ir,
        function: "patch",
        arguments: [.int(40), .bool(true)]
    )

    #expect(result == .int(42))
}

@Test func interpretNamedVoidFunction() throws {
    let ir = """
    define void @patch(i64 %value) {
    entry:
      ret void
    }
    """

    let result = try LLVMIRInterpreter().run(
        ir: ir,
        function: "patch",
        arguments: [.int(1)]
    )

    #expect(result == .void)
}

@Test func rejectNamedFunctionArgumentMismatch() throws {
    let ir = """
    define i64 @patch(i64 %value) {
    entry:
      ret i64 %value
    }
    """

    #expect(throws: LLVMIRInterpreterError.self) {
        try LLVMIRInterpreter().run(
            ir: ir,
            function: "patch",
            arguments: [.bool(true)]
        )
    }
}
```

- [ ] **Step 2: Run the suite and verify RED**

Run:

```bash
xcodebuild test -project IR.xcodeproj -scheme IR \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO
```

Expected: FAIL because `LLVMInvocationValue`, `LLVMInvocationResult`, and `run(ir:function:arguments:)` do not exist.

- [ ] **Step 3: Implement the typed public boundary**

Add above `LLVMIRInterpreter` in `IR/LLVMIRInterpreter.swift`:

```swift
enum LLVMInvocationValue: Equatable {
    case int(Int)
    case bool(Bool)
    case pointer(Int)
}

enum LLVMInvocationResult: Equatable {
    case int(Int)
    case bool(Bool)
    case pointer(Int)
    case void
}
```

Add to `LLVMIRInterpreter` and make the existing private value conversion explicit:

```swift
func run(
    ir: String,
    function name: String,
    arguments: [LLVMInvocationValue],
    host: LLVMHostContext? = nil
) throws -> LLVMInvocationResult {
    let module = try LLVMIRParser().parseModule(ir: ir)
    guard let function = module.functions[name] else {
        throw LLVMIRInterpreterError.parse("Missing @\(name) function.")
    }

    let values = arguments.map(LLVMValue.init(invocationValue:))
    let state = LLVMRuntimeState()
    let value = try execute(
        function: function,
        module: module,
        state: state,
        arguments: values,
        host: host
    )
    return try LLVMInvocationResult(value: value, returnType: function.returnType)
}
```

Change `runMain` to call `run(ir:function:arguments:host:)` and require `.int` or `.bool`, preserving its current public behavior.

- [ ] **Step 4: Run tests and verify GREEN**

Run the same `xcodebuild test` command.

Expected: all existing and new tests PASS.

- [ ] **Step 5: Commit**

```bash
git add IR/LLVMIRInterpreter.swift IRTests/IRTests.swift
git commit -m "feat: interpret typed LLVM patch entries"
```

## Task 3: Replace Patch Points With Target and Signature IDs

**Files:**
- Modify: `IR/Hotfix.swift:3-104`
- Test: `IRTests/IRTests.swift:388-417`

- [ ] **Step 1: Replace the old executor test with failing registry tests**

Add tests that use unique `UserDefaults` suites:

```swift
@Test func managerActivatesPatchByTargetID() throws {
    let defaults = UserDefaults(suiteName: "ir.hotfix.\(UUID().uuidString)")!
    let manager = HotfixManager(userDefaults: defaults, storageKey: "state")
    let patch = HotfixPatch(
        id: "math.v1",
        targetID: 100,
        signatureID: 200,
        entryFunction: "patch",
        ir: "define i64 @patch(i64 %x) { entry: ret i64 %x }"
    )

    manager.upsert(patch)
    try manager.activatePatch(id: patch.id)

    #expect(manager.activePatch(for: 100) == patch)
}

@Test func hotfixIDsUseStableFNV1aAndCanonicalSignatures() {
    #expect(HotfixID.fnv1a64("") == 14_695_981_039_346_656_037)
    #expect(HotfixID.fnv1a64("a") == 12_638_187_200_555_641_996)
    #expect(
        HotfixID.signature(
            returnKind: .int,
            argumentKinds: [.int, .bool],
            hasReceiver: true
        ) == HotfixID.fnv1a64("return=i64;arguments=i64,i1;receiver=1")
    )
}

@Test func managerDeactivatesOnlyRequestedTarget() throws {
    let defaults = UserDefaults(suiteName: "ir.hotfix.\(UUID().uuidString)")!
    let manager = HotfixManager(userDefaults: defaults, storageKey: "state")
    let first = HotfixPatch(id: "a", targetID: 1, signatureID: 10,
                            entryFunction: "patch", ir: "")
    let second = HotfixPatch(id: "b", targetID: 2, signatureID: 20,
                             entryFunction: "patch", ir: "")
    manager.upsert(first)
    manager.upsert(second)
    try manager.activatePatch(id: "a")
    try manager.activatePatch(id: "b")

    manager.deactivatePatch(for: 1)

    #expect(manager.activePatch(for: 1) == nil)
    #expect(manager.activePatch(for: 2) == second)
}

@Test func managerPublishesWholeSnapshotsDuringConcurrentActivation() throws {
    let defaults = UserDefaults(suiteName: "ir.hotfix.\(UUID().uuidString)")!
    let manager = HotfixManager(userDefaults: defaults, storageKey: "state")
    let first = HotfixPatch(id: "a", targetID: 1, signatureID: 10,
                            entryFunction: "patch", ir: "")
    let second = HotfixPatch(id: "b", targetID: 1, signatureID: 10,
                             entryFunction: "patch", ir: "")
    manager.upsert(first)
    manager.upsert(second)
    try manager.activatePatch(id: "a")
    let failureLock = NSLock()
    var sawInvalidSnapshot = false

    DispatchQueue.concurrentPerform(iterations: 200) { index in
        try? manager.activatePatch(id: index.isMultiple(of: 2) ? "a" : "b")
        if let active = manager.activePatch(for: 1), active.id != "a", active.id != "b" {
            failureLock.withLock { sawInvalidSnapshot = true }
        }
    }

    #expect(!sawInvalidSnapshot)
}
```

- [ ] **Step 2: Run tests and verify RED**

Expected: compilation fails because `HotfixPatch` still requires `patchPoint`.

- [ ] **Step 3: Implement the thread-safe target registry**

Replace the patch/state model with:

```swift
struct HotfixPatch: Codable, Equatable {
    let id: String
    let targetID: UInt64
    let signatureID: UInt64
    let entryFunction: String
    let ir: String
}

private struct HotfixState: Codable {
    var patchesByID: [String: HotfixPatch]
    var activePatchIDByTarget: [UInt64: String]
}

private struct HotfixSnapshot {
    var patchesByID: [String: HotfixPatch] = [:]
    var activePatchIDByTarget: [UInt64: String] = [:]
}
```

Add a shared Swift-side identifier implementation used by tests, tooling, and manually-authored patches:

```swift
enum HotfixValueKind: UInt8, Codable {
    case int = 1
    case bool = 2
    case void = 3

    var abiName: String {
        switch self {
        case .int: return "i64"
        case .bool: return "i1"
        case .void: return "void"
        }
    }
}

enum HotfixID {
    static func fnv1a64(_ text: String) -> UInt64 {
        text.utf8.reduce(14_695_981_039_346_656_037) { hash, byte in
            (hash ^ UInt64(byte)) &* 1_099_511_628_211
        }
    }

    static func signature(
        returnKind: HotfixValueKind,
        argumentKinds: [HotfixValueKind],
        hasReceiver: Bool
    ) -> UInt64 {
        let arguments = argumentKinds.map(\.abiName).joined(separator: ",")
        return fnv1a64(
            "return=\(returnKind.abiName);arguments=\(arguments);receiver=\(hasReceiver ? 1 : 0)"
        )
    }
}
```

Use `NSLock.withLock` around snapshot reads and copy-mutate-persist-publish updates. Expose `activePatch(for targetID: UInt64)` and `deactivatePatch(for targetID: UInt64)`.

- [ ] **Step 4: Run tests and verify GREEN**

Expected: registry and all interpreter tests PASS.

- [ ] **Step 5: Commit**

```bash
git add IR/Hotfix.swift IRTests/IRTests.swift
git commit -m "feat: key hotfixes by instrumented target"
```

## Task 4: Add Runtime Invocation and the C Bridge

**Files:**
- Modify: `IR/Hotfix.swift`
- Modify: `IR/LLVMIRInterpreter.swift:20-29`
- Test: `IRTests/IRTests.swift`

- [ ] **Step 1: Write failing runtime success and fallback tests**

```swift
@Test func runtimeExecutesMatchingPatch() throws {
    let defaults = UserDefaults(suiteName: "ir.hotfix.\(UUID().uuidString)")!
    let manager = HotfixManager(userDefaults: defaults, storageKey: "state")
    let patch = HotfixPatch(
        id: "add.v1",
        targetID: 11,
        signatureID: 22,
        entryFunction: "patch",
        ir: """
        define i64 @patch(i64 %x) {
        entry:
          %out = add i64 %x, 2
          ret i64 %out
        }
        """
    )
    manager.upsert(patch)
    try manager.activatePatch(id: patch.id)
    let runtime = HotfixRuntime(manager: manager)

    let result = runtime.invoke(
        targetID: 11,
        signatureID: 22,
        arguments: [.int(40)],
        receiver: nil
    )

    #expect(result == .int(42))
}

@Test func runtimeFallsBackForSignatureMismatch() throws {
    let defaults = UserDefaults(suiteName: "ir.hotfix.\(UUID().uuidString)")!
    let manager = HotfixManager(userDefaults: defaults, storageKey: "state")
    let runtime = HotfixRuntime(manager: manager)

    #expect(runtime.invoke(targetID: 1, signatureID: 2,
                           arguments: [], receiver: nil) == nil)
}

@Test func recursionGuardRejectsOnlyTheActiveTarget() {
    #expect(HotfixRecursionGuard.enter(10))
    defer { HotfixRecursionGuard.leave(10) }
    #expect(!HotfixRecursionGuard.enter(10))
    #expect(HotfixRecursionGuard.enter(11))
    HotfixRecursionGuard.leave(11)
}

@Test func runtimeFallsBackForInvalidIR() throws {
    let defaults = UserDefaults(suiteName: "ir.hotfix.\(UUID().uuidString)")!
    let manager = HotfixManager(userDefaults: defaults, storageKey: "state")
    let patch = HotfixPatch(id: "broken", targetID: 3, signatureID: 4,
                            entryFunction: "patch", ir: "not llvm ir")
    manager.upsert(patch)
    try manager.activatePatch(id: patch.id)

    #expect(HotfixRuntime(manager: manager).invoke(
        targetID: 3, signatureID: 4, arguments: [], receiver: nil
    ) == nil)
}
```

- [ ] **Step 2: Run tests and verify RED**

Expected: `HotfixRuntime` is undefined.

- [ ] **Step 3: Implement runtime invocation and recursion protection**

Add:

```swift
final class HotfixRuntime {
    static let shared = HotfixRuntime(manager: .shared)

    private let manager: HotfixManager
    private let interpreter: LLVMIRInterpreter

    init(manager: HotfixManager, interpreter: LLVMIRInterpreter = .init()) {
        self.manager = manager
        self.interpreter = interpreter
    }

    func invoke(
        targetID: UInt64,
        signatureID: UInt64,
        arguments: [LLVMInvocationValue],
        receiver: AnyObject?
    ) -> LLVMInvocationResult? {
        guard let patch = manager.activePatch(for: targetID),
              patch.signatureID == signatureID,
              HotfixRecursionGuard.enter(targetID) else { return nil }
        defer { HotfixRecursionGuard.leave(targetID) }

        var invocationArguments = arguments
        if receiver != nil { invocationArguments.insert(.pointer(0), at: 0) }
        return try? interpreter.run(
            ir: patch.ir,
            function: patch.entryFunction,
            arguments: invocationArguments,
            host: LLVMHostContext(rootObject: receiver)
        )
    }
}
```

Rename the host initializer parameter to `rootObject` while retaining the UIKit convenience initializer used by current callers. Implement `HotfixRecursionGuard` with `Thread.current.threadDictionary` and a private key.

- [ ] **Step 4: Write the failing raw-buffer bridge test**

Register a matching patch on `HotfixManager.shared`, allocate `UInt8` kinds, `UInt64` arguments/result, call `ir_hotfix_invoke`, and assert it returns `true` with result `42`. Deactivate the unique target in `defer`.

- [ ] **Step 5: Implement the C ABI bridge**

```swift
@_cdecl("ir_hotfix_invoke")
func ir_hotfix_invoke(
    _ targetID: UInt64,
    _ signatureID: UInt64,
    _ argumentKinds: UnsafePointer<UInt8>?,
    _ argumentBits: UnsafePointer<UInt64>?,
    _ argumentCount: Int32,
    _ receiver: UnsafeRawPointer?,
    _ resultBits: UnsafeMutablePointer<UInt64>?
) -> Bool {
    guard argumentCount >= 0 else { return false }
    let count = Int(argumentCount)
    let arguments = HotfixArgumentDecoder.decode(
        kinds: argumentKinds,
        bits: argumentBits,
        count: count
    )
    guard let arguments else { return false }

    let object = receiver.map {
        Unmanaged<AnyObject>.fromOpaque($0).takeUnretainedValue()
    }
    guard let result = HotfixRuntime.shared.invoke(
        targetID: targetID,
        signatureID: signatureID,
        arguments: arguments,
        receiver: object
    ) else { return false }

    return HotfixResultEncoder.write(result, to: resultBits)
}
```

Use kind `1` for `i64` and kind `2` for `i1`; encode signed integers with bit-pattern preserving conversions.

- [ ] **Step 6: Run tests and verify GREEN**

Expected: direct runtime and raw C buffer tests PASS.

- [ ] **Step 7: Commit**

```bash
git add IR/Hotfix.swift IR/LLVMIRInterpreter.swift IRTests/IRTests.swift
git commit -m "feat: execute typed patches through C bridge"
```

## Task 5: Instrument Scalar Swift Functions

**Files:**
- Modify: `Tools/HotfixPass/Sources/HotfixPass.cpp`
- Create: `Tools/HotfixPass/Tests/fixtures/scalars.ll`
- Create: `Tools/HotfixPass/Tests/run-pass-tests.sh`

- [ ] **Step 1: Write the failing scalar pass fixture and assertions**

Create fixture functions for `swiftcc i64 (i64)`, `swiftcc i1 (i1)`, and `swiftcc void (i64)`, plus an unsupported `double` function. In `run-pass-tests.sh`, run:

```bash
"/opt/homebrew/opt/llvm@19/bin/opt" \
  -load-pass-plugin "$PLUGIN" \
  -passes=hotfix-instrument \
  -S "$FIXTURE" -o "$OUTPUT"
```

Assert with `FileCheck` directives:

```llvm
; CHECK: define swiftcc i64 @integerTarget(i64 %value)
; CHECK: call i1 @ir_hotfix_invoke
; CHECK: define private swiftcc i64 @integerTarget.hotfix_original
; CHECK: define swiftcc double @unsupported(double %value)
; CHECK-NOT: @unsupported.hotfix_original
```

Run `Tools/HotfixPass/Tests/run-pass-tests.sh`.

Expected: FAIL because the named pipeline pass and transformation are absent.

- [ ] **Step 2: Implement eligibility and stable IDs**

Add helpers:

```cpp
static uint64_t fnv1a64(StringRef text) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : text.bytes()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

static bool isScalar(Type *type) {
  return type->isIntegerTy(64) || type->isIntegerTy(1);
}
```

Compute signature IDs from the exact canonical text used by `HotfixID.signature`:

```text
return=<void|i1|i64>;arguments=<comma-separated i1/i64>;receiver=<0|1>
```

Eligible functions must be definitions using `CallingConv::Swift`, must not be generated clones or runtime functions, must have supported returns, and must have only scalar arguments except for one optional `swiftself ptr` argument.

- [ ] **Step 3: Implement clone and trampoline generation**

Use `CloneFunction`, rename/link the clone privately, clear the original body, create argument kind/bit arrays with `IRBuilder`, call:

```llvm
declare i1 @ir_hotfix_invoke(i64, i64, ptr, ptr, i32, ptr, ptr)
```

Branch on the result, load and convert the patched result, or call the clone with the untouched original arguments. Copy the original calling convention and attribute list onto the fallback call.

- [ ] **Step 4: Register both named and automatic pipelines**

```cpp
builder.registerPipelineParsingCallback(
    [](StringRef name, ModulePassManager &manager,
       ArrayRef<PassBuilder::PipelineElement>) {
      if (name != "hotfix-instrument") { return false; }
      manager.addPass(HotfixPass());
      return true;
    });
builder.registerPipelineStartEPCallback(
    [](ModulePassManager &manager, OptimizationLevel) {
      manager.addPass(HotfixPass());
    });
```

Use a module flag to guarantee the transformation runs once when both paths are accidentally present.

- [ ] **Step 5: Run pass and Swift-load tests**

Run:

```bash
Tools/HotfixPass/Tests/run-pass-tests.sh
Tools/HotfixPass/Tests/verify-swift-load.sh
```

Expected: all scalar checks PASS, and the Swift fixture contains `ir_hotfix_invoke` and `.hotfix_original`.

- [ ] **Step 6: Commit**

```bash
git add Tools/HotfixPass
git commit -m "feat: instrument scalar Swift functions"
```

## Task 6: Handle Class Receivers, Skip Reasons, and Descriptors

**Files:**
- Modify: `Tools/HotfixPass/Sources/HotfixPass.cpp`
- Modify: `Tools/HotfixPass/Tests/fixtures/scalars.ll`
- Modify: `Tools/HotfixPass/Tests/fixtures/swift_fixture.swift`
- Modify: `Tools/HotfixPass/Tests/run-pass-tests.sh`

- [ ] **Step 1: Add failing receiver and metadata checks**

Add a `swiftcc i64 (i64, ptr swiftself)` fixture and assert:

```llvm
; CHECK: define swiftcc i64 @instanceTarget(i64 %value, ptr swiftself %self)
; CHECK: call i1 @ir_hotfix_invoke({{.*}}ptr %self{{.*}})
; CHECK: @__hotfix_descriptor
; CHECK-SAME: section "__DATA,__hotfix"
```

Add an unsupported pointer argument and assert the output contains the original function only. Capture `opt` stderr and assert it contains `unsupported non-receiver pointer argument`.

- [ ] **Step 2: Run and verify RED**

Expected: receiver, descriptor, and diagnostic assertions fail.

- [ ] **Step 3: Implement receiver detection and marshalling**

Detect `Attribute::SwiftSelf` on exactly one pointer parameter, exclude it from scalar buffers, and pass it as the bridge receiver. Reject every other pointer parameter in the first version. The clone fallback call receives the original `swiftself` value and attributes unchanged.

- [ ] **Step 4: Emit fixed-layout descriptors**

Emit one private constant per target in `__DATA,__hotfix` containing target ID, signature ID, return kind, argument count, receiver flag, and a pointer to a private mangled-name string. Add `llvm.used` references so the optimizer and linker retain the records.

- [ ] **Step 5: Emit deterministic skip diagnostics**

Print one line per defined, non-runtime Swift function skipped by eligibility:

```text
[HotfixPass] skip <symbol>: <reason>
```

Do not diagnose external declarations, LLVM intrinsics, runtime bridge symbols, or generated clones.

- [ ] **Step 6: Run both pass suites and verify GREEN**

Expected: scalar, receiver, descriptor, diagnostic, and Swift frontend checks PASS.

- [ ] **Step 7: Commit**

```bash
git add Tools/HotfixPass
git commit -m "feat: describe instrumented Swift methods"
```

## Task 7: Integrate the Plugin With the App

**Files:**
- Create: `IR/HotfixDemo.swift`
- Modify: `IR/ViewController.swift`
- Modify: `IR.xcodeproj/project.pbxproj`
- Test: `IRTests/IRTests.swift`

- [ ] **Step 1: Add a failing app-level hotfix test**

Create `IR/HotfixDemo.swift`:

```swift
func hotfixableAdd(_ value: Int) -> Int {
    value + 1
}

final class HotfixableCalculator {
    func multiply(_ value: Int) -> Int {
        value * 2
    }
}
```

Add a test that asserts the unpatched values are `42` and then activates matching patches by the IDs emitted for these symbols and asserts new results. Before project integration, the second assertion must fail because calls do not traverse a trampoline.

Use the deterministic identifiers rather than copied numeric literals:

```swift
let addTargetID = HotfixID.fnv1a64("$s2IR13hotfixableAddyS2iF")
let addSignatureID = HotfixID.signature(
    returnKind: .int,
    argumentKinds: [.int],
    hasReceiver: false
)
```

- [ ] **Step 2: Run the app tests and verify RED**

Expected: native results work, activated patch does not alter them.

- [ ] **Step 3: Add an idempotent plugin build script phase**

Add a `PBXShellScriptBuildPhase` before `Sources` in the `IR` target. Its script must:

```bash
set -euo pipefail
LLVM_CONFIG=/opt/homebrew/opt/llvm@19/bin/llvm-config
test "$($LLVM_CONFIG --version)" = "19.1.7"
cmake -S "$SRCROOT/Tools/HotfixPass" \
  -B "$DERIVED_FILE_DIR/HotfixPass" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$DERIVED_FILE_DIR/HotfixPass"
mkdir -p "$DERIVED_FILE_DIR/HotfixPassProduct"
cp "$SRCROOT/Tools/HotfixPass/.build/libHotfixPass.dylib" \
  "$DERIVED_FILE_DIR/HotfixPassProduct/libHotfixPass.dylib"
```

Declare the dylib as the phase output and disable user script sandboxing only for the app target because CMake needs its build directory.

- [ ] **Step 4: Load the pass in Debug only**

Set the app target Debug configuration:

```text
OTHER_SWIFT_FLAGS = $(inherited) -load-pass-plugin=$(DERIVED_FILE_DIR)/HotfixPassProduct/libHotfixPass.dylib
```

Leave Release unchanged. Add pass ignore options for `Hotfix.swift` and `LLVMIRInterpreter.swift` through plugin command-line options or compiled defaults.

- [ ] **Step 5: Replace the manual ViewController demo**

Remove the embedded fallback IR and call the instrumented demo function. Register and activate a matching patch only from the explicit demo bootstrap, print native/patched values, then deactivate it. Keep UI setup independent from the hotfix demonstration.

- [ ] **Step 6: Run tests and inspect emitted IR**

Run:

```bash
xcodebuild test -project IR.xcodeproj -scheme IR \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO

xcrun swiftc -O -emit-ir \
  -load-pass-plugin=Tools/HotfixPass/.build/libHotfixPass.dylib \
  IR/HotfixDemo.swift -o /tmp/HotfixDemo.instrumented.ll
rg 'ir_hotfix_invoke|hotfix_original|__DATA,__hotfix' \
  /tmp/HotfixDemo.instrumented.ll
```

Expected: app tests PASS and the emitted IR shows the trampoline, native clone, and descriptor section.

- [ ] **Step 7: Commit**

```bash
git add IR/HotfixDemo.swift IR/ViewController.swift IRTests/IRTests.swift IR.xcodeproj/project.pbxproj
git commit -m "feat: enable automatic hotfix instrumentation"
```

## Task 8: Document and Verify the Complete Workflow

**Files:**
- Create: `Tools/HotfixPass/README.md`
- Modify: `docs/superpowers/specs/2026-08-14-swift-llvm-hotfix-instrumentation-design.md` only if verified implementation constraints differ.

- [ ] **Step 1: Write workflow documentation**

Document these exact commands:

```bash
brew install llvm@19 cmake
cmake -S Tools/HotfixPass -B Tools/HotfixPass/.cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build Tools/HotfixPass/.cmake-build
Tools/HotfixPass/Tests/run-pass-tests.sh
Tools/HotfixPass/Tests/verify-swift-load.sh
xcodebuild test -project IR.xcodeproj -scheme IR \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO
```

Include the `HotfixPatch` JSON fields, patch entry IR examples, supported ABI table, default exclusions, and Xcode/LLVM version mismatch behavior.

- [ ] **Step 2: Run clean verification**

```bash
rm -rf Tools/HotfixPass/.cmake-build Tools/HotfixPass/.build
cmake -S Tools/HotfixPass -B Tools/HotfixPass/.cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build Tools/HotfixPass/.cmake-build
Tools/HotfixPass/Tests/run-pass-tests.sh
Tools/HotfixPass/Tests/verify-swift-load.sh
xcodebuild clean test -project IR.xcodeproj -scheme IR \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO
git diff --check
```

Expected: every command succeeds with no unexpected warnings or whitespace errors.

- [ ] **Step 3: Inspect the final diff for scope and generated artifacts**

Run:

```bash
git status --short
git diff --stat HEAD~4
find Tools/HotfixPass -type f | sort
```

Expected: no `.cmake-build`, `.build`, object, dylib, DerivedData, or temporary IR files are tracked.

- [ ] **Step 4: Commit documentation**

```bash
git add Tools/HotfixPass/README.md .gitignore
git commit -m "docs: explain Swift hotfix pass workflow"
```

- [ ] **Step 5: Request code review**

Use the `superpowers:requesting-code-review` skill. Address correctness findings, rerun the complete verification command, and only then mark implementation complete.
