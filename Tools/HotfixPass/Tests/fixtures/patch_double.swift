func hotfixPatch(_ value: Double) -> Double {
    let adjusted = value > 10 ? value / 2 : value * 1.5
    return adjusted + 0.25
}
