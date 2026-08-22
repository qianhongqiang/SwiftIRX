//
//  IRTests.swift
//  IRTests
//
//  Created by hongqiang qian on 2026/3/18.
//

import Foundation
import Testing
@testable import IR
#if canImport(UIKit)
import UIKit
#endif

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

private final class SynchronizedBridgeResult: @unchecked Sendable {
    private let lock = NSLock()
    private var invocation: (invoked: Bool, bits: UInt64)?

    func record(invoked: Bool, bits: UInt64) {
        lock.lock()
        invocation = (invoked, bits)
        lock.unlock()
    }

    var value: (invoked: Bool, bits: UInt64)? {
        lock.lock()
        defer { lock.unlock() }
        return invocation
    }
}

struct IRTests {

    @Test func directTopLevelCallUsesActiveHotfix() throws {
        let suiteName = "ir.hotfix.app.add.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let textPatch = """
            define i64 @"$s2IR13hotfixableAddyS2iF"(i64 %value) {
            entry:
              %result = add i64 %value, 10
              ret i64 %result
            }
            """
        let runtime = HotfixRuntime(manager: manager)

        try HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            #expect(hotfixableAdd(41) == 42)
            let activation = try manager.installAndActivate(textPatch: textPatch)
            defer { manager.deactivate(activation) }
            #expect(hotfixableAdd(41) == 51)
        }
    }

    @Test func directClassMethodCallUsesActiveHotfix() throws {
        let suiteName = "ir.hotfix.app.multiply.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let textPatch = """
            define i64 @"$s2IR20HotfixableCalculatorC8multiplyyS2iF"(ptr %self, i64 %value) {
            entry:
              %result = add i64 %value, 100
              ret i64 %result
            }
            """
        let runtime = HotfixRuntime(manager: manager)
        let calculator = HotfixableCalculator()

        try HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            #expect(calculator.multiply(21) == 42)
            let activation = try manager.installAndActivate(textPatch: textPatch)
            defer { manager.deactivate(activation) }
            #expect(calculator.multiply(21) == 121)
        }
    }

    @Test func textPatchDerivesMetadataFromDefinedFunction() throws {
        let symbol = "$s4Demo8replaceyS2iF"
        let textPatch = """
        define i64 @"\(symbol)"(ptr %self, i64 %value, i1 %enabled) {
        entry:
          ret i64 %value
        }
        """

        let patch = try HotfixPatch(text: textPatch)

        #expect(patch.targetID == HotfixID.fnv1a64(symbol))
        #expect(
            patch.signatureID == HotfixID.signature(
                returnKind: .int,
                argumentKinds: [.int, .bool],
                hasReceiver: true
            )
        )
        #expect(patch.entryFunction == symbol)
        #expect(patch.id.hasPrefix("text."))
    }

    @Test func textPatchRejectsMultipleDefinedFunctions() {
        let textPatch = """
        define i64 @first(i64 %value) {
        entry:
          ret i64 %value
        }
        define i64 @second(i64 %value) {
        entry:
          ret i64 %value
        }
        """

        #expect(throws: HotfixTextPatchError.expectedSingleFunction(actualCount: 2)) {
            try HotfixPatch(text: textPatch)
        }
    }

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

    @Test func interpretNamedEntryWrappingAdd() throws {
        let ir = """
        define i64 @patch(i64 %value) {
        entry:
          %result = add i64 %value, 1
          ret i64 %result
        }
        """

        let result = try LLVMIRInterpreter().run(
            ir: ir,
            function: "patch",
            arguments: [.int(Int.max)]
        )

        #expect(result == .int(Int.min))
    }

    @Test func rejectNamedEntrySignedDivisionOverflow() {
        let ir = """
        define i32 @patch(i32 %value) {
        entry:
          %result = sdiv i32 %value, -1
          ret i32 %result
        }
        """

        #expect(throws: LLVMIRInterpreterError.self) {
            try LLVMIRInterpreter().run(
                ir: ir,
                function: "patch",
                arguments: [.int(Int.min)]
            )
        }
    }

    @Test func interpreterRunsAcrossDetachedTaskBoundary() async throws {
        let interpreter = LLVMIRInterpreter()
        let result = try await Task.detached {
            try interpreter.run(
                ir: """
                define i64 @patch(i64 %value) {
                entry:
                  ret i64 %value
                }
                """,
                function: "patch",
                arguments: [.int(42)]
            )
        }.value

        #expect(result == .int(42))
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

    @Test func rejectDuplicateBasicBlockLabels() {
        let ir = """
        define i64 @patch() {
        entry:
          br label %entry
        entry:
          ret i64 1
        }
        """

        #expect(throws: LLVMIRInterpreterError.parse("Duplicate basic block label %entry in function @patch.")) {
            try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
        }
    }

    @Test func rejectRecursiveFunctionCall() {
        let ir = """
        define i64 @patch() {
        entry:
          %result = call i64 @patch()
          ret i64 %result
        }
        """

        #expect(throws: LLVMIRInterpreterError.runtime("Call depth limit exceeded.")) {
            try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
        }
    }

    @Test func rejectMutuallyRecursiveFunctionCalls() {
        let ir = """
        define i64 @patch() {
        entry:
          %result = call i64 @helper()
          ret i64 %result
        }

        define i64 @helper() {
        entry:
          %result = call i64 @patch()
          ret i64 %result
        }
        """

        #expect(throws: LLVMIRInterpreterError.runtime("Call depth limit exceeded.")) {
            try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
        }
    }

    @Test func finiteFunctionCallChainSucceeds() throws {
        let ir = """
        define i64 @patch() {
        entry:
          %result = call i64 @first()
          ret i64 %result
        }

        define i64 @first() {
        entry:
          %result = call i64 @second()
          ret i64 %result
        }

        define i64 @second() {
        entry:
          ret i64 42
        }
        """

        let result = try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])

        #expect(result == .int(42))
    }

    @Test func functionCallsShareExecutionBudget() {
        let ir = """
        define i64 @patch() {
        entry:
          %first = call i64 @spin(i64 99999)
          %second = call i64 @spin(i64 99999)
          ret i64 %second
        }

        define i64 @spin(i64 %limit) {
        entry:
          br label %loop
        loop:
          %value = phi i64 [ 0, %entry ], [ %next, %loop ]
          %next = add i64 %value, 1
          %done = icmp eq i64 %next, %limit
          br i1 %done, label %exit, label %loop
        exit:
          ret i64 %next
        }
        """

        #expect(throws: LLVMIRInterpreterError.runtime("Step limit exceeded. Possible infinite loop.")) {
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

    @MainActor
    @Test func genericObjCInvokerMarshalsIntegerAndBoolValues() throws {
        let ir = """
        define i64 @patch(ptr %self) {
        entry:
          %setTagSelector = load ptr, ptr @"\\01L_selector(setTag:)"
          call void @objc_msgSend(ptr %self, ptr %setTagSelector, i64 73)
          %setHiddenSelector = load ptr, ptr @"\\01L_selector(setHidden:)"
          call void @objc_msgSend(ptr %self, ptr %setHiddenSelector, i1 true)
          %tagSelector = load ptr, ptr @"\\01L_selector(tag)"
          %tag = call i64 @objc_msgSend(ptr %self, ptr %tagSelector)
          %hiddenSelector = load ptr, ptr @"\\01L_selector(isHidden)"
          %hidden = call i1 @objc_msgSend(ptr %self, ptr %hiddenSelector)
          br i1 %hidden, label %hiddenResult, label %visibleResult
        hiddenResult:
          %result = add i64 %tag, 1
          ret i64 %result
        visibleResult:
          ret i64 %tag
        }
        """
        let view = UIView()

        let result = try LLVMIRInterpreter().run(
            ir: ir,
            function: "patch",
            arguments: [.pointer(0)],
            host: LLVMHostContext(rootObject: view)
        )

        #expect(result == .int(74))
        #expect(view.tag == 73)
        #expect(view.isHidden)
    }

    @MainActor
    @Test func objcNilReceiverDoesNotAliasHostRoot() throws {
        let ir = """
        define i1 @patch(ptr %self) {
        entry:
          %superviewSelector = load ptr, ptr @"\\01L_selector(superview)"
          %nullableSuperview = call ptr @objc_msgSend(ptr %self, ptr %superviewSelector)
          %setHiddenSelector = load ptr, ptr @"\\01L_selector(setHidden:)"
          call void @objc_msgSend(ptr %nullableSuperview, ptr %setHiddenSelector, i1 true)
          %hiddenSelector = load ptr, ptr @"\\01L_selector(isHidden)"
          %hidden = call i1 @objc_msgSend(ptr %self, ptr %hiddenSelector)
          ret i1 %hidden
        }
        """
        let view = UIView()

        let result = try LLVMIRInterpreter().run(
            ir: ir,
            function: "patch",
            arguments: [.pointer(0)],
            host: LLVMHostContext(rootObject: view)
        )

        #expect(result == .bool(false))
        #expect(!view.isHidden)
    }

    @Test func unknownExternalFunctionFailsInsteadOfReturningDefaultValue() {
        let ir = """
        define i64 @patch() {
        entry:
          %value = call i64 @unknown_runtime_function()
          ret i64 %value
        }
        """

        #expect(throws: LLVMIRInterpreterError.runtime(
            "Call to unknown function @unknown_runtime_function."
        )) {
            try LLVMIRInterpreter().run(
                ir: ir,
                function: "patch",
                arguments: []
            )
        }
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

    @Test func rejectUnsafeInsertValueIndices() {
        let indices = ["-1", String(Int.max), "1024", "1000000"]

        for index in indices {
            let ir = """
            define i64 @patch() {
            entry:
              %value = insertvalue %aggregate undef, i64 42, \(index)
              ret i64 0
            }
            """

            #expect(throws: LLVMIRInterpreterError.runtime("insertvalue index out of range.")) {
                try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
            }
        }
    }

    @Test func rejectUnsafeInsertValueIndexWhenGrowingAggregate() {
        let ir = """
        define i64 @patch() {
        entry:
          %first = insertvalue %aggregate undef, i64 1, 0
          %second = insertvalue %aggregate %first, i64 2, 1024
          ret i64 0
        }
        """

        #expect(throws: LLVMIRInterpreterError.runtime("insertvalue index out of range.")) {
            try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
        }
    }

    @Test func rejectUnsafeFloatingIntegerOperands() {
        let operands = ["nan", "inf", "-inf", "1e999", "9.223372036854776e18", "1.5"]

        for type in ["i32", "i64"] {
            for operand in operands {
                let ir = """
                define \(type) @patch() {
                entry:
                  ret \(type) \(operand)
                }
                """

                #expect(throws: LLVMIRInterpreterError.parse("Invalid \(type) operand: \(operand)")) {
                    try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])
                }
            }
        }
    }

    @Test func interpretExactFloatingIntegerOperands() throws {
        let ir = """
        define i64 @patch() {
        entry:
          %zero = add i64 0, 0.000e+00
          %result = add i64 %zero, 100e+00
          ret i64 %result
        }
        """

        let result = try LLVMIRInterpreter().run(ir: ir, function: "patch", arguments: [])

        #expect(result == .int(100))
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

    @MainActor
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

        let viewController = UIViewController()
        _ = viewController.view
        let result = try LLVMIRInterpreter().run(
            ir: ir,
            function: "$s14ViewControllerAAC7setupUI33_37ACD668159BB52851391EE68C0B8918LLyyF",
            arguments: [.pointer(0)],
            host: LLVMHostContext(rootViewController: viewController)
        )

        #expect(result == .void)
        #expect(viewController.view.subviews.count == 1)
        #expect(viewController.view.subviews.first?.frame == CGRect(x: 0, y: 0, width: 100, height: 100))
        #expect(viewController.view.subviews.first?.backgroundColor == UIColor.red)
    }

    @MainActor
    @Test func installSetupUIFromBundledTextPatch() throws {
        guard let patchURL = Bundle.main.url(forResource: "HotfixSetupUI", withExtension: "irpatch") else {
            throw CocoaError(.fileNoSuchFile)
        }
        let textPatch = try String(contentsOf: patchURL, encoding: .utf8)
        let patch = try HotfixPatch(text: textPatch)
        let suiteName = "ir.hotfix.setup-ui.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let activation = try manager.installAndActivate(textPatch: textPatch)
        defer { manager.deactivate(activation) }
        let viewController = UIViewController()
        _ = viewController.view

        let result = HotfixRuntime(manager: manager).invoke(
            targetID: patch.targetID,
            signatureID: patch.signatureID,
            arguments: [],
            receiver: viewController
        )

        #expect(result == .void)
        #expect(viewController.view.subviews.count == 1)
        #expect(viewController.view.subviews.first?.frame == CGRect(x: 0, y: 0, width: 100, height: 100))
        #expect(viewController.view.subviews.first?.backgroundColor == UIColor.red)
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

    @Test func runtimeExecutesMatchingPatch() throws {
        let suiteName = "ir.hotfix.runtime.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.runtime",
            targetID: 11,
            signatureID: 22,
            entryFunction: "patch",
            ir: """
            define i64 @patch(i64 %value) {
            entry:
              %result = add i64 %value, 2
              ret i64 %result
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)

        let result = HotfixRuntime(manager: manager).invoke(
            targetID: 11,
            signatureID: 22,
            arguments: [.int(40)],
            receiver: nil
        )

        #expect(result == .int(42))
    }

    @Test func runtimeFallsBackForSignatureMismatch() throws {
        let suiteName = "ir.hotfix.runtime.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.signature",
            targetID: 11,
            signatureID: 22,
            entryFunction: "patch",
            ir: "define i64 @patch(i64 %value) { ret i64 %value }"
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)
        let runtime = HotfixRuntime(manager: manager)

        #expect(runtime.invoke(targetID: 11, signatureID: 23, arguments: [.int(40)], receiver: nil) == nil)
        #expect(runtime.invoke(targetID: 12, signatureID: 22, arguments: [.int(40)], receiver: nil) == nil)
    }

    @Test func runtimeFallsBackForInvalidIR() throws {
        let suiteName = "ir.hotfix.runtime.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.invalid",
            targetID: 11,
            signatureID: 22,
            entryFunction: "patch",
            ir: "not llvm ir"
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)

        let result = HotfixRuntime(manager: manager).invoke(
            targetID: 11,
            signatureID: 22,
            arguments: [.int(40)],
            receiver: nil
        )

        #expect(result == nil)
    }

    @Test func malformedPatchFallsBackWithoutChangingBridgeResult() throws {
        let suiteName = "ir.hotfix.malformed.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.malformed",
            targetID: 31,
            signatureID: 32,
            entryFunction: "patch",
            ir: """
            define i64 @patch() {
            entry:
              br label %entry
            entry:
              ret i64 1
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)
        let runtime = HotfixRuntime(manager: manager)

        #expect(runtime.invoke(targetID: 31, signatureID: 32, arguments: [], receiver: nil) == nil)
        HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            var result: UInt64 = 0xBEEF
            #expect(!ir_hotfix_invoke(31, 32, nil, nil, 0, nil, &result))
            #expect(result == 0xBEEF)
        }
    }

    @Test func recursivePatchFallsBackWithoutChangingBridgeResult() throws {
        let suiteName = "ir.hotfix.recursive.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.recursive",
            targetID: 33,
            signatureID: 34,
            entryFunction: "patch",
            ir: """
            define i64 @patch() {
            entry:
              %result = call i64 @patch()
              ret i64 %result
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)
        let runtime = HotfixRuntime(manager: manager)

        #expect(runtime.invoke(targetID: 33, signatureID: 34, arguments: [], receiver: nil) == nil)
        HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            var result: UInt64 = 0xCAFE
            #expect(!ir_hotfix_invoke(33, 34, nil, nil, 0, nil, &result))
            #expect(result == 0xCAFE)
        }
    }

    @Test func unsafeOperandPatchesFallBackWithoutChangingBridgeResult() throws {
        let suiteName = "ir.hotfix.unsafe-operands.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patches = [
            HotfixPatch(
                id: "patch.unsafe-index",
                targetID: 35,
                signatureID: 36,
                entryFunction: "patch",
                ir: """
                define i64 @patch() {
                entry:
                  %value = insertvalue %aggregate undef, i64 42, -1
                  ret i64 0
                }
                """
            ),
            HotfixPatch(
                id: "patch.unsafe-floating",
                targetID: 37,
                signatureID: 38,
                entryFunction: "patch",
                ir: """
                define i64 @patch() {
                entry:
                  ret i64 nan
                }
                """
            )
        ]
        for patch in patches {
            manager.upsert(patch)
            try manager.activatePatch(id: patch.id)
        }
        let runtime = HotfixRuntime(manager: manager)

        #expect(runtime.invoke(targetID: 35, signatureID: 36, arguments: [], receiver: nil) == nil)
        #expect(runtime.invoke(targetID: 37, signatureID: 38, arguments: [], receiver: nil) == nil)
        HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            var indexResult: UInt64 = 0xCAFE
            #expect(!ir_hotfix_invoke(35, 36, nil, nil, 0, nil, &indexResult))
            #expect(indexResult == 0xCAFE)

            var floatingResult: UInt64 = 0xBEEF
            #expect(!ir_hotfix_invoke(37, 38, nil, nil, 0, nil, &floatingResult))
            #expect(floatingResult == 0xBEEF)
        }
    }

    @Test func recursionGuardRejectsOnlyActiveTarget() {
        let guardState = HotfixRecursionGuard()

        #expect(guardState.enter(10))
        #expect(!guardState.enter(10))
        #expect(guardState.enter(11))

        guardState.leave(11)
        guardState.leave(10)
        #expect(guardState.enter(10))
        guardState.leave(10)
    }

    @Test func runtimePassesSyntheticPointerAndReceiverHost() throws {
        let suiteName = "ir.hotfix.runtime.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.receiver",
            targetID: 11,
            signatureID: 22,
            entryFunction: "patch",
            ir: """
            define i64 @patch(ptr %self, i64 %value) {
            entry:
              %hasReceiver = icmp ne ptr %self, null
              br i1 %hasReceiver, label %receiverPresent, label %receiverMissing
            receiverPresent:
              ret i64 %value
            receiverMissing:
              ret i64 0
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)
        let receiver = NSObject()

        let result = HotfixRuntime(manager: manager).invoke(
            targetID: 11,
            signatureID: 22,
            arguments: [.int(42)],
            receiver: receiver
        )

        #expect(result == .int(42))
    }

    @Test func publicFrameABIHasStableVersionOneLayouts() {
        #expect(HotfixABI.version == 1)
        #expect(MemoryLayout<HFHandle>.size == 16)
        #expect(MemoryLayout<HFValue>.size == 32)
        #expect(MemoryLayout<HFPatchFrame>.size == 96)
        #expect(MemoryLayout<HFDescriptor>.size == 56)
    }

    @Test func frameGatewayValidatesInvokesAndEncodesResult() throws {
        let suiteName = "ir.hotfix.frame.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.frame.int",
            targetID: 81,
            signatureID: 82,
            entryFunction: "patch",
            ir: """
            define i64 @patch(i64 %value) {
            entry:
              %result = add i64 %value, 2
              ret i64 %result
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)

        func makeFrame(targetID: UInt64 = 81) -> HFPatchFrame {
            var frame = HFPatchFrame()
            frame.abiVersion = HotfixABI.version
            frame.structSize = UInt32(MemoryLayout<HFPatchFrame>.size)
            frame.targetID = targetID
            frame.signatureID = 82
            frame.receiver.kind = HotfixABI.invalidHandleKind
            frame.result.kind = HotfixABI.invalidKind
            frame.status = HFStatus(HFStatusInvalidFrame)
            return frame
        }

        let runtime = HotfixRuntime(manager: manager)
        HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            var argument = HFValue()
            argument.kind = HotfixABI.signedIntegerKind
            argument.bits = 40
            var frame = makeFrame()
            frame.argumentCount = 1
            let status = withUnsafePointer(to: &argument) { pointer in
                frame.arguments = pointer
                return hf_vm_invoke(&frame)
            }
            #expect(status == HFStatus(HFStatusApplied))
            #expect(frame.status == HFStatus(HFStatusApplied))
            #expect(frame.result.kind == HotfixABI.signedIntegerKind)
            #expect(frame.result.bits == 42)

            var missing = makeFrame(targetID: 999)
            #expect(hf_vm_invoke(&missing) == HFStatus(HFStatusNoPatch))
            #expect(missing.status == HFStatus(HFStatusNoPatch))
            #expect(missing.result.kind == HotfixABI.invalidKind)

            var wrongVersion = makeFrame()
            wrongVersion.abiVersion = HotfixABI.version + 1
            #expect(hf_vm_invoke(&wrongVersion) == HFStatus(HFStatusABIVersionMismatch))
            #expect(wrongVersion.status == HFStatus(HFStatusABIVersionMismatch))

            var malformed = makeFrame()
            malformed.structSize = 0
            #expect(hf_vm_invoke(&malformed) == HFStatus(HFStatusInvalidFrame))
            #expect(malformed.status == HFStatus(HFStatusInvalidFrame))

            var unexpectedArguments = makeFrame()
            withUnsafePointer(to: &argument) { pointer in
                unexpectedArguments.arguments = pointer
                #expect(hf_vm_invoke(&unexpectedArguments) == HFStatus(HFStatusInvalidArguments))
            }
            #expect(unexpectedArguments.status == HFStatus(HFStatusInvalidArguments))
        }
    }

    @Test func rawCBridgeDecodesAndEncodesSupportedValues() throws {
        let suiteName = "ir.hotfix.bridge.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patches = [
            HotfixPatch(
                id: "patch.bridge.int",
                targetID: 11,
                signatureID: 22,
                entryFunction: "patch",
                ir: """
                define i64 @patch(i64 %value) {
                entry:
                  %result = add i64 %value, 2
                  ret i64 %result
                }
                """
            ),
            HotfixPatch(
                id: "patch.bridge.bool",
                targetID: 12,
                signatureID: 23,
                entryFunction: "patch",
                ir: """
                define i1 @patch(i1 %value) {
                entry:
                  ret i1 %value
                }
                """
            ),
            HotfixPatch(
                id: "patch.bridge.void",
                targetID: 13,
                signatureID: 24,
                entryFunction: "patch",
                ir: """
                define void @patch() {
                entry:
                  ret void
                }
                """
            ),
            HotfixPatch(
                id: "patch.bridge.sdiv-overflow",
                targetID: 14,
                signatureID: 25,
                entryFunction: "patch",
                ir: """
                define i32 @patch(i32 %value) {
                entry:
                  %result = sdiv i32 %value, -1
                  ret i32 %result
                }
                """
            ),
            HotfixPatch(
                id: "patch.bridge.too-many-arguments",
                targetID: 16,
                signatureID: 26,
                entryFunction: "patch",
                ir: """
                define i64 @patch(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h, i64 %i) {
                entry:
                  ret i64 99
                }
                """
            ),
            HotfixPatch(
                id: "patch.bridge.maximum-arguments",
                targetID: 17,
                signatureID: 27,
                entryFunction: "patch",
                ir: """
                define i64 @patch(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h) {
                entry:
                  %result = add i64 %a, %h
                  ret i64 %result
                }
                """
            )
        ]
        for patch in patches {
            manager.upsert(patch)
            try manager.activatePatch(id: patch.id)
        }
        let runtime = HotfixRuntime(manager: manager)
        let previousRuntime = HotfixBridgeRuntime.current

        HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
            var intKind: UInt8 = 1
            var intBits: UInt64 = 40
            var intResult: UInt64 = 0
            #expect(ir_hotfix_invoke(11, 22, &intKind, &intBits, 1, nil, &intResult))
            #expect(intResult == 42)

            var boolKind: UInt8 = 2
            var boolBits: UInt64 = 1
            var boolResult: UInt64 = 0
            #expect(ir_hotfix_invoke(12, 23, &boolKind, &boolBits, 1, nil, &boolResult))
            #expect(boolResult == 1)

            #expect(ir_hotfix_invoke(13, 24, nil, nil, 0, nil, nil))
            #expect(!ir_hotfix_invoke(13, 24, nil, nil, 0, nil, &intResult))

            intBits = UInt64(bitPattern: Int64.min)
            #expect(!ir_hotfix_invoke(14, 25, &intKind, &intBits, 1, nil, &intResult))

            var unknownKind: UInt8 = 99
            var voidKind: UInt8 = 3
            var malformedBits: UInt64 = 0
            #expect(!ir_hotfix_invoke(11, 22, nil, &malformedBits, 1, nil, &intResult))
            #expect(!ir_hotfix_invoke(11, 22, &intKind, nil, 1, nil, &intResult))
            #expect(!ir_hotfix_invoke(11, 22, &unknownKind, &malformedBits, 1, nil, &intResult))
            #expect(!ir_hotfix_invoke(11, 22, &voidKind, &malformedBits, 1, nil, &intResult))
            var negativeCountResult: UInt64 = 0xBEEF
            #expect(!ir_hotfix_invoke(11, 22, &intKind, &intBits, -1, nil, &negativeCountResult))
            #expect(negativeCountResult == 0xBEEF)
            #expect(!ir_hotfix_invoke(11, 22, &intKind, &intBits, 1, nil, nil))

            #expect(HotfixABI.maximumScalarArgumentCount == 8)
            let maximumKinds = Array(repeating: UInt8(1), count: 8)
            let maximumBits: [UInt64] = [40, 0, 0, 0, 0, 0, 0, 2]
            var maximumResult: UInt64 = 0
            let appliedMaximum = maximumKinds.withUnsafeBufferPointer { kinds in
                maximumBits.withUnsafeBufferPointer { bits in
                    ir_hotfix_invoke(17, 27, kinds.baseAddress, bits.baseAddress, 8, nil, &maximumResult)
                }
            }
            #expect(appliedMaximum)
            #expect(maximumResult == 42)

            let tooManyKinds = Array(repeating: UInt8(1), count: 9)
            let tooManyBits = Array(repeating: UInt64(1), count: 9)
            var tooManyResult: UInt64 = 0xCAFE
            let appliedTooMany = tooManyKinds.withUnsafeBufferPointer { kinds in
                tooManyBits.withUnsafeBufferPointer { bits in
                    ir_hotfix_invoke(16, 26, kinds.baseAddress, bits.baseAddress, 9, nil, &tooManyResult)
                }
            }
            #expect(!appliedTooMany)
            #expect(tooManyResult == 0xCAFE)

            var invalidBoolBits: UInt64 = 2
            #expect(!ir_hotfix_invoke(12, 23, &boolKind, &invalidBoolBits, 1, nil, &boolResult))

            var pointerResult: UInt64 = 0
            #expect(!HotfixResultEncoder.encode(.pointer(1), to: &pointerResult))
        }
        #expect(HotfixBridgeRuntime.current === previousRuntime)
    }

#if canImport(UIKit)
    @MainActor
    @Test func rawCBridgePropagatesUnmanagedReceiverToHost() throws {
        let suiteName = "ir.hotfix.bridge.receiver.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.bridge.receiver",
            targetID: 15,
            signatureID: 26,
            entryFunction: "patch",
            ir: """
            define i64 @patch(ptr %self) {
            entry:
              %viewSelector = load ptr, ptr @"\\01L_selector(view)"
              %view = call ptr @objc_msgSend(ptr %self, ptr %viewSelector)
              %colorClass = load ptr, ptr @"OBJC_CLASS_REF_$_UIColor"
              %colorMeta = call ptr @objc_opt_self(ptr %colorClass)
              %redSelector = load ptr, ptr @"\\01L_selector(redColor)"
              %red = call ptr @objc_msgSend(ptr %colorMeta, ptr %redSelector)
              %backgroundSelector = load ptr, ptr @"\\01L_selector(setBackgroundColor:)"
              call void @objc_msgSend(ptr %view, ptr %backgroundSelector, ptr %red)
              ret i64 42
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)
        let receiver = UIViewController()
        let receiverPointer = Unmanaged.passUnretained(receiver).toOpaque()
        var result: UInt64 = 0

        let invoked = HotfixBridgeRuntime.withRuntimeForTesting(HotfixRuntime(manager: manager)) {
            ir_hotfix_invoke(15, 26, nil, nil, 0, receiverPointer, &result)
        }

        #expect(invoked)
        #expect(result == 42)
        #expect(receiver.view.backgroundColor?.isEqual(UIColor.red) == true)
    }

    @MainActor
    @Test func rawCBridgeRejectsUIKitReceiverPatchOffMainThread() throws {
        let suiteName = "ir.hotfix.bridge.background-receiver.tests.\(UUID().uuidString)"
        let userDefaults = UserDefaults(suiteName: suiteName)!
        defer { userDefaults.removePersistentDomain(forName: suiteName) }
        let manager = HotfixManager(userDefaults: userDefaults, storageKey: "state")
        let patch = HotfixPatch(
            id: "patch.bridge.background-receiver",
            targetID: 16,
            signatureID: 27,
            entryFunction: "patch",
            ir: """
            define i64 @patch(ptr %self) {
            entry:
              %viewSelector = load ptr, ptr @"\\01L_selector(view)"
              %view = call ptr @objc_msgSend(ptr %self, ptr %viewSelector)
              %colorClass = load ptr, ptr @"OBJC_CLASS_REF_$_UIColor"
              %colorMeta = call ptr @objc_opt_self(ptr %colorClass)
              %redSelector = load ptr, ptr @"\\01L_selector(redColor)"
              %red = call ptr @objc_msgSend(ptr %colorMeta, ptr %redSelector)
              %backgroundSelector = load ptr, ptr @"\\01L_selector(setBackgroundColor:)"
              call void @objc_msgSend(ptr %view, ptr %backgroundSelector, ptr %red)
              ret i64 42
            }
            """
        )
        manager.upsert(patch)
        try manager.activatePatch(id: patch.id)
        let receiver = UIViewController()
        _ = receiver.view
        let receiverAddress = UInt(bitPattern: Unmanaged.passUnretained(receiver).toOpaque())
        let result = SynchronizedBridgeResult()
        let finished = DispatchSemaphore(value: 0)
        let runtime = HotfixRuntime(manager: manager)

        let worker = Thread {
            var resultBits: UInt64 = 0
            let invoked = HotfixBridgeRuntime.withRuntimeForTesting(runtime) {
                ir_hotfix_invoke(
                    16,
                    27,
                    nil,
                    nil,
                    0,
                    UnsafeRawPointer(bitPattern: receiverAddress),
                    &resultBits
                )
            }
            result.record(invoked: invoked, bits: resultBits)
            finished.signal()
        }
        worker.qualityOfService = .userInteractive
        worker.start()
        #expect(finished.wait(timeout: .now() + 5) == .success)

        #expect(result.value?.invoked == false)
        #expect(result.value?.bits == 0)
        #expect(receiver.view.backgroundColor == nil)
    }
#endif

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
