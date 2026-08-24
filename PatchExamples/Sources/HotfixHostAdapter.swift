@_silgen_name("$s2IR13hotfixableAddyS2iF")
func releasedHotfixableAdd(_ value: Int) -> Int

func hotfixPatch(_ value: Int) -> Int {
    releasedHotfixableAdd(value) * 2
}
