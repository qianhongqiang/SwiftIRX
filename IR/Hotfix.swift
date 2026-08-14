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
        guard let patchID = snapshot.activePatchIDByTarget[targetID] else {
            return nil
        }
        return snapshot.patchesByID[patchID]
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

final class HotfixExecutor {
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
