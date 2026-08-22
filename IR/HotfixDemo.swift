import Foundation

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
