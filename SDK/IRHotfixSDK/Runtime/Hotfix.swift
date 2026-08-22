import Foundation

nonisolated enum HotfixBinaryPatchError: Error, Equatable, Sendable {
    case installFailed(status: HFStatus)
    case activationFailed(status: HFStatus)
}

nonisolated enum HotfixValueKind: UInt8, Codable, Sendable {
    case int = 1
    case bool = 2
    case void = 3

    var abiName: String {
        switch self {
        case .int: "i64"
        case .bool: "i1"
        case .void: "void"
        }
    }
}

nonisolated enum HotfixABI {
    static let version = UInt32(HF_ABI_VERSION)
    static let maximumScalarArgumentCount = Int32(HF_MAX_SCALAR_ARGUMENT_COUNT)

    static let signedIntegerKind = UInt32(HFValueKindSignedInteger)
    static let boolKind = UInt32(HFValueKindBool)
    static let voidKind = UInt32(HFValueKindVoid)
    static let invalidKind = UInt32(HFValueKindInvalid)

    static let hasReceiverFlag = UInt32(HFPatchFrameFlagHasReceiver)
    static let objectHandleKind = UInt16(HFHandleKindObject)
    static let invalidHandleKind = UInt16(HFHandleKindInvalid)
    static let borrowedHandleFlags = UInt16(
        HFHandleFlagBorrowed | HFHandleFlagBorrowedAddress
    )
}

nonisolated enum HotfixTargetValueKind: String, Codable, Sendable {
    case int = "i64"
    case bool = "i1"
    case void
}

nonisolated struct HotfixTargetDescriptor: Codable, Equatable, Sendable {
    let symbol: String
    let targetID: String
    let signatureID: String
    let returnKind: HotfixTargetValueKind
    let argumentKinds: [HotfixTargetValueKind]
    let hasReceiver: Bool

    var targetIDValue: UInt64? { Self.decodeHexID(targetID) }
    var signatureIDValue: UInt64? { Self.decodeHexID(signatureID) }

    private static func decodeHexID(_ value: String) -> UInt64? {
        guard value.count == 18, value.hasPrefix("0x") else { return nil }
        return UInt64(value.dropFirst(2), radix: 16)
    }
}

nonisolated struct HotfixTargetManifest: Codable, Equatable, Sendable {
    static let supportedSchemaVersion: UInt32 = 1
    static let resourceName = "HotfixTargetManifest"

    let schemaVersion: UInt32
    let abiVersion: UInt32
    let targets: [HotfixTargetDescriptor]

    func target(symbol: String) -> HotfixTargetDescriptor? {
        targets.first { $0.symbol == symbol }
    }
}

nonisolated enum HotfixID {
    static func fnv1a64(_ value: String) -> UInt64 {
        value.utf8.reduce(14_695_981_039_346_656_037) { hash, byte in
            (hash ^ UInt64(byte)) &* 1_099_511_628_211
        }
    }

    static func signature(
        returnKind: HotfixValueKind,
        argumentKinds: [HotfixValueKind],
        hasReceiver: Bool
    ) -> UInt64 {
        let arguments = argumentKinds.map(\.abiName).joined(separator: ",")
        let receiver = hasReceiver ? 1 : 0
        return fnv1a64(
            "return=\(returnKind.abiName);arguments=\(arguments);receiver=\(receiver)"
        )
    }
}

nonisolated struct HotfixBinaryActivation: @unchecked Sendable {
    fileprivate let handle: HFIRPatchHandle

    var targetID: UInt64 { handle.targetID }
    var signatureID: UInt64 { handle.signatureID }
}

/// Lifecycle for verified binary HFIR patches. LLVM text is a build-time input
/// only and is never parsed or interpreted in the app.
nonisolated final class HotfixManager: @unchecked Sendable {
    static let shared = HotfixManager()

    init() {}

    @discardableResult
    func installAndActivate(binaryPatch data: Data) throws -> HotfixBinaryActivation {
        var handle = HFIRPatchHandle()
        let installStatus = data.withUnsafeBytes { bytes in
            hf_hfir_vm_install(bytes.baseAddress, bytes.count, &handle)
        }
        guard installStatus == HFStatus(HFStatusApplied) else {
            throw HotfixBinaryPatchError.installFailed(status: installStatus)
        }

        let activationStatus = hf_hfir_vm_activate(handle)
        guard activationStatus == HFStatus(HFStatusApplied) else {
            _ = hf_hfir_vm_uninstall(handle)
            throw HotfixBinaryPatchError.activationFailed(status: activationStatus)
        }
        return HotfixBinaryActivation(handle: handle)
    }

    func deactivate(_ activation: HotfixBinaryActivation) {
        _ = hf_hfir_vm_deactivate(activation.handle)
        _ = hf_hfir_vm_uninstall(activation.handle)
    }
}

/// Stable trampoline target emitted by HotfixPass. Runtime dispatch is binary
/// HFIR-only; there is intentionally no Swift text-interpreter fallback.
@_cdecl("hf_vm_invoke")
nonisolated func hf_vm_invoke(
    _ framePointer: UnsafeMutablePointer<HFPatchFrame>?
) -> HFStatus {
    hf_hfir_vm_invoke(framePointer)
}
