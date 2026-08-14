//
//  IRTests.swift
//  IRTests
//
//  Created by hongqiang qian on 2026/3/18.
//

import Foundation
import Testing
@testable import IR

private final class SynchronizedFailureFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var failed = false

    func recordFailure() {
        lock.lock()
        failed = true
        lock.unlock()
    }

    var value: Bool {
        lock.lock()
        defer { lock.unlock() }
        return failed
    }
}

struct IRTests {

    @Test func interpretNamedFunctionWithTypedArguments() throws {
        let ir = """
        define i64 @patch(i64 %value, i1 %enabled) {
        entry:
          %matches = icmp eq i64 %value, 40
          %apply = and i1 %matches, %enabled
          br i1 %apply, label %enabled, label %disabled
        enabled:
          ret i64 42
        disabled:
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

    @Test func rejectNamedFunctionArgumentMismatch() {
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

    @Test func interpretNamedBooleanResult() throws {
        let ir = """
        define i1 @patch(i64 %value) {
        entry:
          %matches = icmp eq i64 %value, 42
          ret i1 %matches
        }
        """

        let result = try LLVMIRInterpreter().run(
            ir: ir,
            function: "patch",
            arguments: [.int(42)]
        )

        #expect(result == .bool(true))
    }

    @Test func rejectMissingNamedFunction() {
        let ir = """
        define i32 @main() {
        entry:
          ret i32 0
        }
        """

        #expect(throws: LLVMIRInterpreterError.parse("Missing @patch function.")) {
            try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
        }
    }

    @Test func interpretArithmeticProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %a = add i32 40, 2
          %b = mul i32 %a, 2
          %c = sdiv i32 %b, 2
          ret i32 %c
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretBranchAndPhiProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %cond = icmp sgt i32 7, 3
          br i1 %cond, label %then, label %else
        then:
          br label %merge
        else:
          br label %merge
        merge:
          %x = phi i32 [ 10, %then ], [ 20, %else ]
          ret i32 %x
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 10)
    }

    @Test func interpretAllocaLoadStoreProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %ptr = alloca i32
          store i32 21, ptr %ptr
          %v = load i32, ptr %ptr
          %out = mul i32 %v, 2
          ret i32 %out
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretFunctionCallProgram() throws {
        let ir = """
        define i32 @double(i32 %x) {
        entry:
          %out = mul i32 %x, 2
          ret i32 %out
        }

        define i32 @main() {
        entry:
          %v = call i32 @double(i32 21)
          ret i32 %v
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretBitwiseAndShiftProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %a = and i32 15, 6
          %b = xor i32 %a, 1
          %c = shl i32 %b, 2
          %d = ashr i32 %c, 1
          ret i32 %d
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 14)
    }

    @Test func interpretCastProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %a = zext i1 true to i32
          %b = sext i1 true to i32
          %c = add i32 %a, %b
          %d = trunc i32 %c to i1
          %e = zext i1 %d to i32
          ret i32 %e
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 0)
    }

    @Test func interpretVoidCallProgram() throws {
        let ir = """
        define void @set(i32 %v) {
        entry:
          ret void
        }

        define i32 @main() {
        entry:
          call void @set(i32 123)
          ret i32 42
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretPointerIntRoundTripProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %p = alloca i32
          %n = ptrtoint ptr %p to i64
          %q = inttoptr i64 %n to ptr
          store i32 42, ptr %q
          %v = load i32, ptr %p
          ret i32 %v
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretIcmpI64Program() throws {
        let ir = """
        define i32 @main() {
        entry:
          %eq = icmp eq i64 1234, 1234
          %v = zext i1 %eq to i32
          ret i32 %v
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 1)
    }

    @Test func interpretExternalCallStubsProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %p = alloca i32
          call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 8, i1 false)
          %r = call ptr @llvm.objc.retain(ptr %p)
          %n = ptrtoint ptr %r to i64
          %q = inttoptr i64 %n to ptr
          store i32 42, ptr %q
          %v = load i32, ptr %p
          ret i32 %v
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretCallWithConventionAndAttributesProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %p = alloca i32
          %r = call swiftcc ptr @objc_opt_self(ptr nocapture %p)
          %ok = icmp eq ptr %r, %p
          %out = zext i1 %ok to i32
          ret i32 %out
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 1)
    }

    @Test func interpretExtractValueFallbackProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %p = alloca i32
          %x = extractvalue ptr %p, 0
          %n = ptrtoint ptr %x to i64
          %q = inttoptr i64 %n to ptr
          store i32 42, ptr %q
          %v = load i32, ptr %p
          ret i32 %v
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretInlineAsmProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %x = call i32 asm sideeffect "nop", ""()
          %y = add i32 %x, 42
          ret i32 %y
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func interpretExtractValueTypedAggregateSyntaxProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %1 = call ptr @llvm.objc.retain(ptr null)
          %2 = extractvalue %swift.metadata_response %1, 0
          %ok = icmp eq ptr %2, %1
          %out = zext i1 %ok to i32
          ret i32 %out
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 1)
    }

    @Test func interpretInsertValueAndExtractValueProgram() throws {
        let ir = """
        define ptr @make() {
        entry:
          %p = alloca i32
          %a = insertvalue %swift.metadata_response undef, ptr %p, 0
          %b = insertvalue %swift.metadata_response %a, i64 0, 1
          ret %swift.metadata_response %b
        }

        define i32 @main() {
        entry:
          %v = call ptr @make()
          %ok = icmp ne ptr %v, null
          %out = zext i1 %ok to i32
          ret i32 %out
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 1)
    }

    @Test func interpretGetElementPtrProgram() throws {
        let ir = """
        define i32 @main() {
        entry:
          %base = alloca ptr
          %f0 = getelementptr inbounds %objc_super, ptr %base, i32 0, i32 0
          %f1 = getelementptr inbounds %objc_super, ptr %base, i32 0, i32 1
          store ptr null, ptr %f0
          store ptr %base, ptr %f1
          %v = load ptr, ptr %f1
          %ok = icmp eq ptr %v, %base
          %out = zext i1 %ok to i32
          ret i32 %out
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 1)
    }

    @Test func interpretFullSetupUIProgram() throws {
        let ir = """
        define hidden swiftcc void @"$s14ViewControllerAAC7setupUI33_37ACD668159BB52851391EE68C0B8918LLyyF"(ptr swiftself %0) #0 {
        entry:
          %self.debug = alloca ptr, align 8
          call void @llvm.memset.p0.i64(ptr align 8 %self.debug, i8 0, i64 8, i1 false)
          %view.debug = alloca ptr, align 8
          call void @llvm.memset.p0.i64(ptr align 8 %view.debug, i8 0, i64 8, i1 false)
          store ptr %0, ptr %self.debug, align 8
          %1 = call swiftcc %swift.metadata_response @"$sSo6UIViewCMa"(i64 0)
          %2 = extractvalue %swift.metadata_response %1, 0
          %3 = call swiftcc ptr @"$sSo6UIViewC5frameABSo6CGRectV_tcfC"(double 0.000000e+00, double 0.000000e+00, double 1.000000e+02, double 1.000000e+02, ptr swiftself %2)
          store ptr %3, ptr %view.debug, align 8
          %4 = load ptr, ptr @"OBJC_CLASS_REF_$_UIColor", align 8
          %5 = call ptr @objc_opt_self(ptr %4)
          %6 = load ptr, ptr @"\\01L_selector(redColor)", align 8
          %7 = call ptr @objc_msgSend(ptr %5, ptr %6)
          call void asm sideeffect "mov\\09fp, fp\\09\\09// marker for objc_retainAutoreleaseReturnValue", ""()
          %8 = call ptr @llvm.objc.retainAutoreleasedReturnValue(ptr %7)
          %9 = ptrtoint ptr %8 to i64
          %10 = load ptr, ptr @"\\01L_selector(setBackgroundColor:)", align 8
          %11 = inttoptr i64 %9 to ptr
          call void @objc_msgSend(ptr %3, ptr %10, ptr %11)
          %12 = inttoptr i64 %9 to ptr
          call void @llvm.objc.release(ptr %12)
          %13 = call ptr @llvm.objc.retain(ptr %0)
          %14 = load ptr, ptr @"\\01L_selector(view)", align 8
          %15 = call ptr @objc_msgSend(ptr %0, ptr %14)
          call void asm sideeffect "mov\\09fp, fp\\09\\09// marker for objc_retainAutoreleaseReturnValue", ""()
          %16 = call ptr @llvm.objc.retainAutoreleasedReturnValue(ptr %15)
          %17 = ptrtoint ptr %16 to i64
          call void @llvm.objc.release(ptr %0)
          %18 = icmp eq i64 %17, 0
          br i1 %18, label %21, label %19

        19:
          %20 = inttoptr i64 %17 to ptr
          br label %22

        21:
          call swiftcc void @"$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_A2HSus6UInt32VtF"(i64 ptrtoint (ptr @".str.11.Fatal error" to i64), i64 11, i8 2, i64 ptrtoint (ptr @".str.68.Unexpectedly found nil while implicitly unwrapping an Optional value" to i64), i64 68, i8 2, i64 ptrtoint (ptr @".str.35.ViewController/ViewController.swift" to i64), i64 35, i8 2, i64 21, i32 0)
          unreachable

        22:
          %23 = phi ptr [ %20, %19 ]
          %24 = load ptr, ptr @"\\01L_selector(addSubview:)", align 8
          call void @objc_msgSend(ptr %23, ptr %24, ptr %3)
          call void @llvm.objc.release(ptr %23)
          call void @llvm.objc.release(ptr %3)
          ret void
        }

        define i32 @main() {
        entry:
          call swiftcc void @"$s14ViewControllerAAC7setupUI33_37ACD668159BB52851391EE68C0B8918LLyyF"(ptr null)
          ret i32 42
        }
        """

        let interpreter = LLVMIRInterpreter()
        let result = try interpreter.runMain(ir: ir)

        #expect(result == 42)
    }

    @Test func hotfixExecutorUsesActivePatch() throws {
        let userDefaults = UserDefaults(suiteName: "ir.hotfix.tests.\(UUID().uuidString)")!
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.v1",
            patchPoint: "demo.point",
            ir: """
            define i32 @main() {
            entry:
              ret i32 7
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)

        let executor = HotfixExecutor(manager: manager, interpreter: LLVMIRInterpreter())
        let execution = try executor.runMain(
            patchPoint: "demo.point",
            fallbackIR: """
            define i32 @main() {
            entry:
              ret i32 42
            }
            """
        )

        #expect(execution.result == 7)
        #expect(execution.patchID == "patch.v1")
    }

    @Test func managerActivatesPatchByTargetID() throws {
        let suiteName = "ir.hotfix.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.target.v1",
            targetID: 0x1020_3040_5060_7080,
            signatureID: 0x8877_6655_4433_2211,
            entryFunction: "patchedFunction",
            ir: "define i64 @patchedFunction() { ret i64 42 }"
        )

        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)

        #expect(manager.activePatch(for: patch.targetID) == patch)
    }

    @Test func managerDeactivatesOnlyRequestedTarget() throws {
        let suiteName = "ir.hotfix.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let first = HotfixPatch(
            id: "patch.first",
            targetID: 101,
            signatureID: 201,
            entryFunction: "first",
            ir: "first IR"
        )
        let second = HotfixPatch(
            id: "patch.second",
            targetID: 102,
            signatureID: 202,
            entryFunction: "second",
            ir: "second IR"
        )
        manager.upsert(first)
        manager.upsert(second)
        try manager.activatePatch(id: first.id)
        try manager.activatePatch(id: second.id)

        manager.deactivatePatch(for: first.targetID)

        #expect(manager.activePatch(for: first.targetID) == nil)
        #expect(manager.activePatch(for: second.targetID) == second)
    }

    @Test func managerRetargetingPatchDeactivatesOldTarget() throws {
        let suiteName = "ir.hotfix.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let original = HotfixPatch(
            id: "patch.retargeted",
            targetID: 501,
            signatureID: 601,
            entryFunction: "original",
            ir: "original IR"
        )
        let replacement = HotfixPatch(
            id: original.id,
            targetID: 502,
            signatureID: 602,
            entryFunction: "replacement",
            ir: "replacement IR"
        )
        manager.upsert(original)
        try manager.activatePatch(id: original.id)

        manager.upsert(replacement)

        #expect(manager.activePatch(for: original.targetID) == nil)
        #expect(manager.activePatch(for: replacement.targetID) == nil)

        try manager.activatePatch(id: replacement.id)

        #expect(manager.activePatch(for: replacement.targetID) == replacement)
    }

    @Test func fnv1a64UsesStandardOffsetBasisForEmptyInput() {
        #expect(HotfixID.fnv1a64("") == 14_695_981_039_346_656_037)
    }

    @Test func fnv1a64HashesUTF8Bytes() {
        #expect(HotfixID.fnv1a64("a") == 12_638_187_200_555_641_996)
    }

    @Test func signatureIDUsesCanonicalABIString() {
        let canonical = "return=i64;arguments=i64,i1;receiver=1"

        #expect(
            HotfixID.signature(
                returnKind: .int,
                argumentKinds: [.int, .bool],
                hasReceiver: true
            ) == HotfixID.fnv1a64(canonical)
        )
    }

    @Test func concurrentActivationAndReadUseCompleteSnapshots() throws {
        let suiteName = "ir.hotfix.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let first = HotfixPatch(
            id: "patch.concurrent.first",
            targetID: 301,
            signatureID: 401,
            entryFunction: "first",
            ir: "first IR"
        )
        let second = HotfixPatch(
            id: "patch.concurrent.second",
            targetID: first.targetID,
            signatureID: 402,
            entryFunction: "second",
            ir: "second IR"
        )
        manager.upsert(first)
        manager.upsert(second)
        try manager.activatePatch(id: first.id)
        let failure = SynchronizedFailureFlag()

        DispatchQueue.concurrentPerform(iterations: 1_000) { iteration in
            do {
                if iteration.isMultiple(of: 2) {
                    try manager.activatePatch(id: first.id)
                } else {
                    try manager.activatePatch(id: second.id)
                }
            } catch {
                failure.recordFailure()
                return
            }

            guard let active = manager.activePatch(for: first.targetID),
                  active == first || active == second else {
                failure.recordFailure()
                return
            }
        }

        #expect(!failure.value)
    }
}
