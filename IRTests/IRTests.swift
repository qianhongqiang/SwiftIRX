import Foundation
import Testing
@testable import IR
#if canImport(UIKit)
import UIKit
#endif

struct IRTests {
    @Test func publicFrameABIHasStableVersionThreeLayouts() {
        #expect(HotfixABI.version == 3)
        #expect(MemoryLayout<HFHandle>.size == 16)
        #expect(MemoryLayout<HFValue>.size == 32)
        #expect(MemoryLayout<HFPatchFrame>.size == 96)
        #expect(MemoryLayout<HFDescriptor>.size == 56)
        #expect(MemoryLayout<HFHostCallDescriptor>.size == 80)
        #expect(MemoryLayout<HFHostCallFrame>.size == 96)
    }

    @Test func stableIDsUseCanonicalABIText() {
        #expect(HotfixID.fnv1a64("") == 14_695_981_039_346_656_037)
        #expect(HotfixID.fnv1a64("a") == 12_638_187_200_555_641_996)
        #expect(HotfixID.signature(
            returnKind: .int,
            argumentKinds: [.int, .bool],
            hasReceiver: true
        ) == HotfixID.fnv1a64("return=i64;arguments=i64,i1;receiver=1"))
    }

    @Test func bundledTargetManifestMatchesRuntimeABI() throws {
        let url = try #require(Bundle.main.url(
            forResource: HotfixTargetManifest.resourceName,
            withExtension: "json"
        ))
        let manifest = try JSONDecoder().decode(
            HotfixTargetManifest.self,
            from: Data(contentsOf: url)
        )

        #expect(manifest.schemaVersion == HotfixTargetManifest.supportedSchemaVersion)
        #expect(manifest.abiVersion == HotfixABI.version)
        #expect(manifest.targets.contains {
            $0.symbol.contains("ViewControllerC7setupUI") &&
                $0.returnKind == .void &&
                $0.argumentKinds.isEmpty &&
                $0.hasReceiver
        })
        #expect(manifest.target(
            symbol: "hotfix_example_c_add"
        )?.effectiveReceiverKind == HotfixTargetReceiverKind.none)
        #expect(manifest.target(
            symbol: "_ZN12HFCalculator8multiplyEx"
        )?.effectiveReceiverKind == .native)
    }

    @Test func bundledHostAdapterManifestMatchesGeneratedGateways() throws {
        let url = try #require(Bundle.main.url(
            forResource: HotfixHostAdapterManifest.resourceName,
            withExtension: "json"
        ))
        let manifest = try JSONDecoder().decode(
            HotfixHostAdapterManifest.self,
            from: Data(contentsOf: url)
        )

        #expect(manifest.schemaVersion == HotfixHostAdapterManifest.supportedSchemaVersion)
        #expect(manifest.abiVersion == UInt32(HF_HOST_ADAPTER_ABI_VERSION))
        #expect(manifest.adapter(symbol: "$s2IR13hotfixableAddyS2iF")?.language == .swift)
        #expect(manifest.adapter(
            symbol: "$s2IR20HotfixableCalculatorC8multiplyyS2iF"
        )?.hasReceiver == true)
    }

    @Test func binaryGatewayRejectsOldABIAndMissingPatch() {
        var oldFrame = HFMakePatchFrame()
        oldFrame.abiVersion = 1
        #expect(hf_vm_invoke(&oldFrame) == HFStatus(HFStatusABIVersionMismatch))
        #expect(oldFrame.status == HFStatus(HFStatusABIVersionMismatch))

        var missingFrame = HFMakePatchFrame()
        missingFrame.targetID = 0xfedc_ba98_7654_3210
        missingFrame.signatureID = 0x0123_4567_89ab_cdef
        #expect(hf_vm_invoke(&missingFrame) == HFStatus(HFStatusNoPatch))
        #expect(missingFrame.status == HFStatus(HFStatusNoPatch))
    }

#if canImport(UIKit)
    @MainActor
    @Test func bundledBinaryPatchExecutesUIKitProgram() throws {
        let patchURL = try #require(Bundle.main.url(
            forResource: "HotfixSetupUI",
            withExtension: "hfpatch"
        ))
        let manager = HotfixManager()
        let activation = try manager.installAndActivate(
            binaryPatch: Data(contentsOf: patchURL)
        )
        defer { manager.deactivate(activation) }

        let viewController = UIViewController()
        _ = viewController.view
        var frame = HFMakePatchFrame()
        frame.targetID = 0x1232093bb65a3a2b
        frame.signatureID = 0x988cc6a5a1ee0058
        frame.flags = HotfixABI.hasReceiverFlag
        #expect(hf_host_handle_scope_begin(
            Unmanaged.passUnretained(viewController).toOpaque(),
            HFHandleKind(HFHandleKindObject),
            &frame.receiver
        ) == HFStatus(HFStatusApplied))
        defer { _ = hf_host_handle_scope_end(frame.receiver) }

        #expect(hf_vm_invoke(&frame) == HFStatus(HFStatusApplied))
        #expect(frame.result.kind == HotfixABI.voidKind)
        let patchedView = try #require(viewController.view.subviews.first)
        #expect(patchedView.frame == CGRect(x: 100, y: 100, width: 100, height: 100))
        #expect(patchedView.backgroundColor == UIColor.yellow)
        let label = try #require(patchedView.subviews.first as? UILabel)
        #expect(label.frame == CGRect(x: 10, y: 10, width: 80, height: 20))
        #expect(label.text == "hello")
    }
#endif

    @Test func swiftHostAdapterUsesSharedDescriptorGateway() throws {
        let symbol = "ir.tests.swift.double"
        let call = HotfixSwiftHostCall(
            symbol: symbol,
            returnKind: HFValueKind(HFValueKindSignedInteger),
            argumentKinds: [HFValueKind(HFValueKindSignedInteger)],
            noSideEffects: true
        )
        let registration = try HotfixSwiftHostAdapter.register(call) { _, arguments in
            HFMakeValue(HFValueKind(HFValueKindSignedInteger), arguments[0].bits &* 2)
        }
        defer { registration.unregister() }

        let argumentKinds = [HFValueKind(HFValueKindSignedInteger)]
        var descriptor = HFHostCallDescriptor()
        var argument = HFMakeValue(HFValueKind(HFValueKindSignedInteger), 21)
        var result = HFMakeValue(HFValueKind(HFValueKindInvalid), 0)
        let status = symbol.withCString { symbolPointer in
            argumentKinds.withUnsafeBufferPointer { kinds in
                descriptor.abiVersion = UInt32(HF_HOST_ADAPTER_ABI_VERSION)
                descriptor.structSize = UInt32(MemoryLayout<HFHostCallDescriptor>.size)
                descriptor.importID = hf_host_call_id(symbolPointer)
                descriptor.language = HFHostLanguage(HFHostLanguageSwift)
                descriptor.callKind = HFHostCallKind(HFHostCallKindFunction)
                descriptor.returnKind = HFValueKind(HFValueKindSignedInteger)
                descriptor.argumentCount = 1
                descriptor.flags = UInt32(HFHostCallFlagNoSideEffects)
                descriptor.name = symbolPointer
                descriptor.argumentKinds = kinds.baseAddress
                descriptor.signatureID = hf_host_call_signature_id(&descriptor)
                return hf_host_adapter_invoke(
                    &descriptor,
                    HFInvalidHandle(),
                    &argument,
                    1,
                    &result
                )
            }
        }

        #expect(status == HFStatus(HFStatusApplied))
        #expect(result.kind == HFValueKind(HFValueKindSignedInteger))
        #expect(result.bits == 42)
    }

    @Test func generatedSwiftHostAdapterRegistersWithoutHandwrittenCode() {
        _ = HotfixManager()
        let symbol = "$s2IR13hotfixableAddyS2iF"
        let argumentKinds = [HFValueKind(HFValueKindSignedInteger)]
        var descriptor = HFHostCallDescriptor()
        var argument = HFMakeValue(HFValueKind(HFValueKindSignedInteger), 21)
        var result = HFMakeValue(HFValueKind(HFValueKindInvalid), 0)
        let status = symbol.withCString { symbolPointer in
            argumentKinds.withUnsafeBufferPointer { kinds in
                descriptor.abiVersion = UInt32(HF_HOST_ADAPTER_ABI_VERSION)
                descriptor.structSize = UInt32(MemoryLayout<HFHostCallDescriptor>.size)
                descriptor.importID = hf_host_call_id(symbolPointer)
                descriptor.language = HFHostLanguage(HFHostLanguageSwift)
                descriptor.callKind = HFHostCallKind(HFHostCallKindFunction)
                descriptor.returnKind = HFValueKind(HFValueKindSignedInteger)
                descriptor.argumentCount = 1
                descriptor.flags = UInt32(HFHostCallFlagNoSideEffects)
                descriptor.name = symbolPointer
                descriptor.argumentKinds = kinds.baseAddress
                descriptor.signatureID = hf_host_call_signature_id(&descriptor)
                return hf_host_adapter_invoke(
                    &descriptor,
                    HFInvalidHandle(),
                    &argument,
                    1,
                    &result
                )
            }
        }

        #expect(status == HFStatus(HFStatusApplied))
        #expect(result.bits == 22)
    }

    @Test func bundledSyntaxExamplePatchesExecuteThroughReleaseTrampolines() throws {
        let manager = HotfixManager.shared

        func withPatch<T>(
            _ resource: String,
            run: () -> T
        ) throws -> T {
            let url = try #require(Bundle.main.url(
                forResource: resource,
                withExtension: "hfpatch"
            ))
            let activation = try manager.installAndActivate(
                binaryPatch: Data(contentsOf: url)
            )
            defer { manager.deactivate(activation) }
            return run()
        }

        #expect(try withPatch("HotfixInteger") {
            hotfixExampleInteger(10)
        } == 110)
        #expect(try withPatch("HotfixBranch") {
            hotfixExampleBranch(55)
        } == true)
        #expect(try withPatch("HotfixInstance") {
            HotfixExampleCalculator().multiply(8)
        } == 24)
        #expect(try withPatch("HotfixHostAdapter") {
            hotfixExampleHostAdapter(10)
        } == 22)
        #expect(try withPatch("HotfixC") {
            hotfix_example_c_add(10)
        } == 210)
        #expect(try withPatch("HotfixCXX") {
            hotfix_example_cxx_multiply(8)
        } == 40)
    }
}
