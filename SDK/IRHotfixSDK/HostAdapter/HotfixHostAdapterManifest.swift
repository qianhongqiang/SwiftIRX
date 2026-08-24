import Foundation

nonisolated enum HotfixHostAdapterLanguage: String, Codable, Sendable {
    case swift
    case c
    case cxx
}

nonisolated enum HotfixHostAdapterCallKind: String, Codable, Sendable {
    case function
    case instanceMethod
    case staticMethod
}

nonisolated enum HotfixHostAdapterValueKind: String, Codable, Sendable {
    case int = "i64"
    case bool = "i1"
    case void
}

nonisolated struct HotfixHostAdapterDescriptor: Codable, Equatable, Sendable {
    let language: HotfixHostAdapterLanguage
    let symbol: String
    let importID: String
    let signatureID: String
    let owner: String
    let callKind: HotfixHostAdapterCallKind
    let returnKind: HotfixHostAdapterValueKind
    let argumentKinds: [HotfixHostAdapterValueKind]
    let hasReceiver: Bool
    let mainThreadOnly: Bool
    let objcCompatibleHandles: Bool
    let noSideEffects: Bool

    var importIDValue: UInt64? { Self.decodeHexID(importID) }
    var signatureIDValue: UInt64? { Self.decodeHexID(signatureID) }

    private static func decodeHexID(_ value: String) -> UInt64? {
        guard value.count == 18, value.hasPrefix("0x") else { return nil }
        return UInt64(value.dropFirst(2), radix: 16)
    }
}

nonisolated struct HotfixHostAdapterManifest: Codable, Equatable, Sendable {
    static let supportedSchemaVersion: UInt32 = 1
    static let resourceName = "HotfixHostAdapterManifest"

    let schemaVersion: UInt32
    let abiVersion: UInt32
    let adapters: [HotfixHostAdapterDescriptor]

    func adapter(symbol: String) -> HotfixHostAdapterDescriptor? {
        adapters.first { $0.symbol == symbol }
    }
}
