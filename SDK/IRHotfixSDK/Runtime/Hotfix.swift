import Foundation

nonisolated enum HotfixError: Error, Equatable, Sendable {
    case patchNotFound(String)
}

nonisolated enum HotfixTextPatchError: Error, Equatable, Sendable {
    case expectedSingleFunction(actualCount: Int)
    case unsupportedReturnType(LLVMIRABIValueKind)
    case unsupportedParameterType(index: Int, kind: LLVMIRABIValueKind)
}

extension HotfixTextPatchError: LocalizedError {
    nonisolated var errorDescription: String? {
        switch self {
        case let .expectedSingleFunction(actualCount):
            return "A text patch must contain exactly one defined function; found \(actualCount)."
        case let .unsupportedReturnType(kind):
            return "Text patch return type \(kind.rawValue) is unsupported."
        case let .unsupportedParameterType(index, kind):
            return "Text patch parameter \(index) has unsupported type \(kind.rawValue)."
        }
    }
}

nonisolated enum HotfixValueKind: UInt8, Codable, Sendable {
    case int = 1
    case bool = 2
    case void = 3

    var abiName: String {
        switch self {
        case .int:
            return "i64"
        case .bool:
            return "i1"
        case .void:
            return "void"
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
    static let borrowedHandleFlags = UInt16(HFHandleFlagBorrowed | HFHandleFlagBorrowedAddress)
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

    var targetIDValue: UInt64? {
        Self.decodeHexID(targetID)
    }

    var signatureIDValue: UInt64? {
        Self.decodeHexID(signatureID)
    }

    private static func decodeHexID(_ value: String) -> UInt64? {
        guard value.count == 18, value.hasPrefix("0x") else {
            return nil
        }
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
        let canonical = "return=\(returnKind.abiName);arguments=\(arguments);receiver=\(hasReceiver ? 1 : 0)"
        return fnv1a64(canonical)
    }
}

nonisolated struct HotfixPatch: Codable, Equatable, Sendable {
    let id: String
    let targetID: UInt64
    let signatureID: UInt64
    let entryFunction: String
    let ir: String

    init(
        id: String,
        targetID: UInt64,
        signatureID: UInt64,
        entryFunction: String,
        ir: String
    ) {
        self.id = id
        self.targetID = targetID
        self.signatureID = signatureID
        self.entryFunction = entryFunction
        self.ir = ir
    }

    init(text: String) throws {
        let descriptors = try LLVMIRInterpreter.functionDescriptors(in: text)
        guard descriptors.count == 1, let descriptor = descriptors.first else {
            throw HotfixTextPatchError.expectedSingleFunction(actualCount: descriptors.count)
        }

        let returnKind: HotfixValueKind
        switch descriptor.returnKind {
        case .i64:
            returnKind = .int
        case .i1:
            returnKind = .bool
        case .void:
            returnKind = .void
        case .i8, .i32, .pointer:
            throw HotfixTextPatchError.unsupportedReturnType(descriptor.returnKind)
        }

        var parameters = descriptor.parameterKinds
        let hasReceiver = parameters.first == .pointer
        if hasReceiver {
            parameters.removeFirst()
        }

        let argumentKinds = try parameters.enumerated().map { index, kind in
            switch kind {
            case .i64:
                return HotfixValueKind.int
            case .i1:
                return HotfixValueKind.bool
            case .i8, .i32, .pointer, .void:
                throw HotfixTextPatchError.unsupportedParameterType(index: index, kind: kind)
            }
        }

        self.init(
            id: "text.\(String(HotfixID.fnv1a64(text), radix: 16))",
            targetID: HotfixID.fnv1a64(descriptor.name),
            signatureID: HotfixID.signature(
                returnKind: returnKind,
                argumentKinds: argumentKinds,
                hasReceiver: hasReceiver
            ),
            entryFunction: descriptor.name,
            ir: text
        )
    }

    // Legacy runMain patches use an integer result with no arguments or receiver.
    init(id: String, patchPoint: String, ir: String) {
        self.init(
            id: id,
            targetID: HotfixID.fnv1a64(patchPoint),
            signatureID: HotfixID.signature(
                returnKind: .int,
                argumentKinds: [],
                hasReceiver: false
            ),
            entryFunction: "main",
            ir: ir
        )
    }
}

nonisolated struct HotfixActivation: Equatable, Sendable {
    fileprivate let targetID: UInt64
    fileprivate let patchID: String
}

private nonisolated struct HotfixState: Codable, Sendable {
    var patchesByID: [String: HotfixPatch]
    var activePatchIDByTarget: [UInt64: String]

    static let empty = HotfixState(
        patchesByID: [:],
        activePatchIDByTarget: [:]
    )
}

nonisolated final class HotfixManager: @unchecked Sendable {
    static let shared = HotfixManager()

    private let userDefaults: UserDefaults
    private let storageKey: String
    private let lock = NSLock()
    private var state: HotfixState

    init(
        userDefaults: UserDefaults = .standard,
        storageKey: String = "ir.hotfix.state"
    ) {
        self.userDefaults = userDefaults
        self.storageKey = storageKey

        if let data = userDefaults.data(forKey: storageKey),
           let decoded = try? JSONDecoder().decode(HotfixState.self, from: data) {
            state = decoded
        } else {
            state = .empty
        }
    }

    func upsert(_ patch: HotfixPatch) {
        lock.lock()
        defer { lock.unlock() }

        var candidate = state
        candidate.activePatchIDByTarget = candidate.activePatchIDByTarget.filter {
            $0.value != patch.id
        }
        candidate.patchesByID[patch.id] = patch
        publish(candidate)
    }

    func activatePatch(id: String) throws {
        lock.lock()
        defer { lock.unlock() }

        guard let patch = state.patchesByID[id] else {
            throw HotfixError.patchNotFound(id)
        }
        var candidate = state
        candidate.activePatchIDByTarget[patch.targetID] = patch.id
        publish(candidate)
    }

    @discardableResult
    func installAndActivate(textPatch: String) throws -> HotfixActivation {
        let patch = try HotfixPatch(text: textPatch)

        lock.lock()
        defer { lock.unlock() }

        var candidate = state
        candidate.activePatchIDByTarget = candidate.activePatchIDByTarget.filter {
            $0.value != patch.id
        }
        candidate.patchesByID[patch.id] = patch
        candidate.activePatchIDByTarget[patch.targetID] = patch.id
        publish(candidate)
        return HotfixActivation(targetID: patch.targetID, patchID: patch.id)
    }

    func deactivate(_ activation: HotfixActivation) {
        lock.lock()
        defer { lock.unlock() }

        guard state.activePatchIDByTarget[activation.targetID] == activation.patchID else {
            return
        }
        var candidate = state
        candidate.activePatchIDByTarget.removeValue(forKey: activation.targetID)
        publish(candidate)
    }

    func deactivatePatch(for targetID: UInt64) {
        lock.lock()
        defer { lock.unlock() }

        var candidate = state
        candidate.activePatchIDByTarget.removeValue(forKey: targetID)
        publish(candidate)
    }

    func activePatch(for targetID: UInt64) -> HotfixPatch? {
        let snapshot = currentSnapshot()
        guard let patchID = snapshot.activePatchIDByTarget[targetID],
              let patch = snapshot.patchesByID[patchID],
              patch.targetID == targetID else {
            return nil
        }
        return patch
    }

    func deactivatePatch(for patchPoint: String) {
        deactivatePatch(for: HotfixID.fnv1a64(patchPoint))
    }

    func activePatch(for patchPoint: String) -> HotfixPatch? {
        activePatch(for: HotfixID.fnv1a64(patchPoint))
    }

    private func currentSnapshot() -> HotfixState {
        lock.lock()
        defer { lock.unlock() }
        return state
    }

    private func publish(_ candidate: HotfixState) {
        do {
            let data = try JSONEncoder().encode(candidate)
            userDefaults.set(data, forKey: storageKey)
            state = candidate
        } catch {
            assertionFailure("Failed to encode hotfix state: \(error)")
        }
    }
}

nonisolated final class HotfixRecursionGuard: @unchecked Sendable {
    private static let threadDictionaryKey = "ir.hotfix.active-targets"

    func enter(_ targetID: UInt64) -> Bool {
        let dictionary = Thread.current.threadDictionary
        var activeTargets = dictionary[Self.threadDictionaryKey] as? Set<UInt64> ?? []
        guard !activeTargets.contains(targetID) else {
            return false
        }
        activeTargets.insert(targetID)
        dictionary[Self.threadDictionaryKey] = activeTargets
        return true
    }

    func leave(_ targetID: UInt64) {
        let dictionary = Thread.current.threadDictionary
        guard var activeTargets = dictionary[Self.threadDictionaryKey] as? Set<UInt64> else {
            return
        }
        activeTargets.remove(targetID)
        if activeTargets.isEmpty {
            dictionary.removeObject(forKey: Self.threadDictionaryKey)
        } else {
            dictionary[Self.threadDictionaryKey] = activeTargets
        }
    }
}

nonisolated final class HotfixRuntime: @unchecked Sendable {
    static let shared = HotfixRuntime(manager: .shared)

    private let manager: HotfixManager
    private let interpreter: LLVMIRInterpreter
    private let recursionGuard: HotfixRecursionGuard

    init(
        manager: HotfixManager = .shared,
        interpreter: LLVMIRInterpreter = LLVMIRInterpreter(),
        recursionGuard: HotfixRecursionGuard = HotfixRecursionGuard()
    ) {
        self.manager = manager
        self.interpreter = interpreter
        self.recursionGuard = recursionGuard
    }

    func invoke(
        targetID: UInt64,
        signatureID: UInt64,
        arguments: [LLVMInvocationValue],
        receiver: AnyObject?
    ) -> LLVMInvocationResult? {
        guard let patch = manager.activePatch(for: targetID),
              patch.signatureID == signatureID,
              recursionGuard.enter(targetID) else {
            return nil
        }
        defer { recursionGuard.leave(targetID) }

        var invocationArguments = arguments
        var host: LLVMHostContext?
        if let receiver {
            invocationArguments.insert(.pointer(0), at: 0)
            host = LLVMHostContext(rootObject: receiver)
        }

        do {
            return try interpreter.run(
                ir: patch.ir,
                function: patch.entryFunction,
                arguments: invocationArguments,
                host: host
            )
        } catch {
#if DEBUG
            print("[HotfixRuntime] Patch \(patch.id) failed for target \(targetID): \(error)")
#endif
            return nil
        }
    }
}

nonisolated enum HotfixArgumentDecoder {
    static func decode(
        kinds: UnsafePointer<UInt8>?,
        bits: UnsafePointer<UInt64>?,
        count: Int32
    ) -> [LLVMInvocationValue]? {
        guard count >= 0, count <= HotfixABI.maximumScalarArgumentCount else {
            return nil
        }
        guard count > 0 else {
            return []
        }
        guard let kinds, let bits else {
            return nil
        }

        var arguments: [LLVMInvocationValue] = []
        arguments.reserveCapacity(Int(count))
        for index in 0..<Int(count) {
            switch HotfixValueKind(rawValue: kinds[index]) {
            case .int:
                arguments.append(.int(Int(bitPattern: UInt(bits[index]))))
            case .bool:
                guard bits[index] <= 1 else {
                    return nil
                }
                arguments.append(.bool(bits[index] == 1))
            case .void, .none:
                return nil
            }
        }
        return arguments
    }
}

nonisolated enum HotfixResultEncoder {
    static func encode(
        _ result: LLVMInvocationResult,
        to resultBits: UnsafeMutablePointer<UInt64>?
    ) -> Bool {
        switch result {
        case let .int(value):
            guard let resultBits else {
                return false
            }
            resultBits.pointee = UInt64(truncatingIfNeeded: value)
            return true
        case let .bool(value):
            guard let resultBits else {
                return false
            }
            resultBits.pointee = value ? 1 : 0
            return true
        case .void:
            return resultBits == nil
        case .pointer:
            return false
        }
    }
}

nonisolated enum HotfixFrameCodec {
    static func decodeArguments(from frame: HFPatchFrame) -> [LLVMInvocationValue]? {
        guard frame.argumentCount <= UInt32(HotfixABI.maximumScalarArgumentCount) else {
            return nil
        }
        guard frame.argumentCount > 0 else {
            guard frame.arguments == nil else {
                return nil
            }
            return []
        }
        guard let arguments = frame.arguments else {
            return nil
        }

        var decoded: [LLVMInvocationValue] = []
        decoded.reserveCapacity(Int(frame.argumentCount))
        for value in UnsafeBufferPointer(start: arguments, count: Int(frame.argumentCount)) {
            guard value.flags == UInt32(HFValueFlagNone),
                  value.bytes == nil,
                  value.byteCount == 0 else {
                return nil
            }
            switch value.kind {
            case HotfixABI.signedIntegerKind:
                decoded.append(.int(Int(bitPattern: UInt(value.bits))))
            case HotfixABI.boolKind:
                guard value.bits <= 1 else {
                    return nil
                }
                decoded.append(.bool(value.bits == 1))
            default:
                return nil
            }
        }
        return decoded
    }

    static func decodeReceiver(from frame: HFPatchFrame) -> AnyObject?? {
        let knownFrameFlags = HotfixABI.hasReceiverFlag
        guard frame.flags & ~knownFrameFlags == 0 else {
            return nil
        }

        let hasReceiver = frame.flags & HotfixABI.hasReceiverFlag != 0
        if !hasReceiver {
            guard frame.receiver.token == 0,
                  frame.receiver.generation == 0,
                  frame.receiver.kind == HotfixABI.invalidHandleKind,
                  frame.receiver.flags == UInt16(HFHandleFlagNone) else {
                return nil
            }
            return .some(nil)
        }

        guard frame.receiver.token != 0,
              frame.receiver.generation == 0,
              frame.receiver.kind == HotfixABI.objectHandleKind,
              frame.receiver.flags == HotfixABI.borrowedHandleFlags,
              let pointer = UnsafeRawPointer(bitPattern: UInt(frame.receiver.token)) else {
            return nil
        }
        return .some(Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue())
    }

    static func encodeResult(_ result: LLVMInvocationResult) -> HFValue? {
        var value = HFValue()
        value.flags = UInt32(HFValueFlagNone)
        value.bytes = nil
        value.byteCount = 0

        switch result {
        case let .int(result):
            value.kind = HotfixABI.signedIntegerKind
            value.bits = UInt64(truncatingIfNeeded: result)
        case let .bool(result):
            value.kind = HotfixABI.boolKind
            value.bits = result ? 1 : 0
        case .void:
            value.kind = HotfixABI.voidKind
            value.bits = 0
        case .pointer:
            return nil
        }
        return value
    }
}

nonisolated enum HotfixBridgeRuntime {
    private final class Storage: @unchecked Sendable {
        let lock = NSRecursiveLock()
        var runtime = HotfixRuntime.shared
    }

    private static let storage = Storage()

    static var current: HotfixRuntime {
        storage.lock.lock()
        defer { storage.lock.unlock() }
        return storage.runtime
    }

    static func withRuntimeForTesting<T>(
        _ runtime: HotfixRuntime,
        perform body: () throws -> T
    ) rethrows -> T {
        storage.lock.lock()
        let previous = storage.runtime
        storage.runtime = runtime
        defer {
            storage.runtime = previous
            storage.lock.unlock()
        }
        return try body()
    }
}

@_cdecl("hf_vm_invoke")
nonisolated func hf_vm_invoke(
    _ framePointer: UnsafeMutablePointer<HFPatchFrame>?
) -> HFStatus {
    guard let framePointer else {
        return HFStatus(HFStatusInvalidFrame)
    }

    var frame = framePointer.pointee
    func finish(_ status: HFStatus) -> HFStatus {
        frame.status = status
        framePointer.pointee = frame
        return status
    }

    guard frame.abiVersion == HotfixABI.version else {
        return finish(HFStatus(HFStatusABIVersionMismatch))
    }
    guard frame.structSize == UInt32(MemoryLayout<HFPatchFrame>.size),
          frame.reserved == 0 else {
        return finish(HFStatus(HFStatusInvalidFrame))
    }
    guard let arguments = HotfixFrameCodec.decodeArguments(from: frame),
          let receiver = HotfixFrameCodec.decodeReceiver(from: frame) else {
        return finish(HFStatus(HFStatusInvalidArguments))
    }
    guard let result = HotfixBridgeRuntime.current.invoke(
        targetID: frame.targetID,
        signatureID: frame.signatureID,
        arguments: arguments,
        receiver: receiver
    ) else {
        return finish(HFStatus(HFStatusNoPatch))
    }
    guard let encoded = HotfixFrameCodec.encodeResult(result) else {
        return finish(HFStatus(HFStatusInvalidResult))
    }

    frame.result = encoded
    return finish(HFStatus(HFStatusApplied))
}

/// Compatibility entry point for existing tests and external prototypes. New
/// pass-generated trampolines call `hf_vm_invoke` with `HFPatchFrame`.
@_cdecl("ir_hotfix_invoke")
nonisolated func ir_hotfix_invoke(
    _ targetID: UInt64,
    _ signatureID: UInt64,
    _ argumentKinds: UnsafePointer<UInt8>?,
    _ argumentBits: UnsafePointer<UInt64>?,
    _ argumentCount: Int32,
    _ receiver: UnsafeRawPointer?,
    _ resultBits: UnsafeMutablePointer<UInt64>?
) -> Bool {
    guard let decoded = HotfixArgumentDecoder.decode(
        kinds: argumentKinds,
        bits: argumentBits,
        count: argumentCount
    ) else {
        return false
    }

    let values: [HFValue] = decoded.map { argument in
        var value = HFValue()
        value.flags = UInt32(HFValueFlagNone)
        switch argument {
        case let .int(bits):
            value.kind = HotfixABI.signedIntegerKind
            value.bits = UInt64(truncatingIfNeeded: bits)
        case let .bool(bits):
            value.kind = HotfixABI.boolKind
            value.bits = bits ? 1 : 0
        case .pointer:
            value.kind = HotfixABI.invalidKind
        }
        return value
    }

    return values.withUnsafeBufferPointer { buffer in
        var frame = HFPatchFrame()
        frame.abiVersion = HotfixABI.version
        frame.structSize = UInt32(MemoryLayout<HFPatchFrame>.size)
        frame.targetID = targetID
        frame.signatureID = signatureID
        frame.arguments = values.isEmpty ? nil : buffer.baseAddress
        frame.argumentCount = UInt32(values.count)
        frame.result.kind = HotfixABI.invalidKind
        frame.status = HFStatus(HFStatusInvalidFrame)

        if let receiver {
            frame.flags = HotfixABI.hasReceiverFlag
            frame.receiver.token = UInt64(UInt(bitPattern: receiver))
            frame.receiver.generation = 0
            frame.receiver.kind = HotfixABI.objectHandleKind
            frame.receiver.flags = HotfixABI.borrowedHandleFlags
        } else {
            frame.flags = UInt32(HFPatchFrameFlagNone)
            frame.receiver.kind = HotfixABI.invalidHandleKind
            frame.receiver.flags = UInt16(HFHandleFlagNone)
        }

        guard hf_vm_invoke(&frame) == HFStatus(HFStatusApplied) else {
            return false
        }
        switch frame.result.kind {
        case HotfixABI.signedIntegerKind:
            guard let resultBits else { return false }
            resultBits.pointee = frame.result.bits
            return true
        case HotfixABI.boolKind:
            guard let resultBits, frame.result.bits <= 1 else { return false }
            resultBits.pointee = frame.result.bits
            return true
        case HotfixABI.voidKind:
            return resultBits == nil
        default:
            return false
        }
    }
}

nonisolated final class HotfixExecutor {
    private let manager: HotfixManager
    private let interpreter: LLVMIRInterpreter

    init(
        manager: HotfixManager = .shared,
        interpreter: LLVMIRInterpreter = LLVMIRInterpreter()
    ) {
        self.manager = manager
        self.interpreter = interpreter
    }

    func runMain(
        patchPoint: String,
        fallbackIR: String,
        host: LLVMHostContext? = nil
    ) throws -> (result: Int, patchID: String?) {
        let targetID = HotfixID.fnv1a64(patchPoint)
        let patch = manager.activePatch(for: targetID)
        let ir = patch?.ir ?? fallbackIR
        let result = try interpreter.runMain(ir: ir, host: host)
        return (result, patch?.id)
    }
}
