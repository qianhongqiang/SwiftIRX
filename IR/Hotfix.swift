import Foundation

enum HotfixError: Error, Equatable {
    case patchNotFound(String)
}

struct HotfixPatch: Codable, Equatable {
    let id: String
    let patchPoint: String
    let ir: String
}

private struct HotfixState: Codable {
    var patchesByID: [String: HotfixPatch]
    var activePatchIDByPoint: [String: String]
}

final class HotfixManager {
    static let shared = HotfixManager()

    private let userDefaults: UserDefaults
    private let storageKey: String

    private var patchesByID: [String: HotfixPatch] = [:]
    private var activePatchIDByPoint: [String: String] = [:]

    init(
        userDefaults: UserDefaults = .standard,
        storageKey: String = "ir.hotfix.state"
    ) {
        self.userDefaults = userDefaults
        self.storageKey = storageKey
        loadState()
    }

    func upsert(_ patch: HotfixPatch) {
        patchesByID[patch.id] = patch
        persistState()
    }

    func activatePatch(id: String) throws {
        guard let patch = patchesByID[id] else {
            throw HotfixError.patchNotFound(id)
        }
        activePatchIDByPoint[patch.patchPoint] = patch.id
        persistState()
    }

    func deactivatePatch(for patchPoint: String) {
        activePatchIDByPoint.removeValue(forKey: patchPoint)
        persistState()
    }

    func activePatch(for patchPoint: String) -> HotfixPatch? {
        guard let patchID = activePatchIDByPoint[patchPoint] else {
            return nil
        }
        return patchesByID[patchID]
    }

    private func loadState() {
        guard let data = userDefaults.data(forKey: storageKey),
              let state = try? JSONDecoder().decode(HotfixState.self, from: data) else {
            return
        }
        patchesByID = state.patchesByID
        activePatchIDByPoint = state.activePatchIDByPoint
    }

    private func persistState() {
        let state = HotfixState(
            patchesByID: patchesByID,
            activePatchIDByPoint: activePatchIDByPoint
        )
        guard let data = try? JSONEncoder().encode(state) else {
            return
        }
        userDefaults.set(data, forKey: storageKey)
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
        let patch = manager.activePatch(for: patchPoint)
        let ir = patch?.ir ?? fallbackIR
        let result = try interpreter.runMain(ir: ir, host: host)
        return (result, patch?.id)
    }
}
