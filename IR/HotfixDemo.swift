import Foundation

nonisolated enum HotfixDemoABI {
    static let addSymbol = "$s2IR13hotfixableAddyS2iF"
    static let multiplySymbol = "$s2IR20HotfixableCalculatorC8multiplyyS2iF"

    static let addTargetID = HotfixID.fnv1a64(addSymbol)
    static let addSignatureID = HotfixID.signature(
        returnKind: .int,
        argumentKinds: [.int],
        hasReceiver: false
    )
    static let multiplyTargetID = HotfixID.fnv1a64(multiplySymbol)
    static let multiplySignatureID = HotfixID.signature(
        returnKind: .int,
        argumentKinds: [.int],
        hasReceiver: true
    )
}

@inline(never)
nonisolated func hotfixableAdd(_ value: Int) -> Int {
    value + 1
}

nonisolated final class HotfixableCalculator {
    @inline(never)
    func multiply(_ value: Int) -> Int {
        value * 2
    }
}
