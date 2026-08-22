func localHelper(_ value: Int) -> Int {
    value + 1
}

func hotfixPatch(_ value: Int) -> Int {
    localHelper(value)
}
