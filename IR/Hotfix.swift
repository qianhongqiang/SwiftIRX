import Foundation

nonisolated enum HotfixError: Error, Equatable, Sendable {
    case patchNotFound(String)
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
    static let maximumScalarArgumentCount: Int32 = 8
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

        return try? interpreter.run(
            ir: patch.ir,
            function: patch.entryFunction,
            arguments: invocationArguments,
            host: host
        )
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
    guard let arguments = HotfixArgumentDecoder.decode(
        kinds: argumentKinds,
        bits: argumentBits,
        count: argumentCount
    ) else {
        return false
    }

    let receiverObject = receiver.map {
        Unmanaged<AnyObject>.fromOpaque($0).takeUnretainedValue()
    }
    guard let result = HotfixBridgeRuntime.current.invoke(
        targetID: targetID,
        signatureID: signatureID,
        arguments: arguments,
        receiver: receiverObject
    ) else {
        return false
    }
    return HotfixResultEncoder.encode(result, to: resultBits)
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
