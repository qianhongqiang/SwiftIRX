func hotfixPatch(_ value: Float) -> Float {
    let adjusted = value > 10 ? value / 2 : value * 1.5
    return adjusted + 0.25
}
