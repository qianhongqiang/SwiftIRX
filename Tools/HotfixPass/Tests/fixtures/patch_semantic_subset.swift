private enum PatchMode {
    case add
    case multiply
    case fallback
}

@inline(never)
private func reachableHelper(_ value: Int) -> Int {
    value + 7
}

func hotfixPatch(_ value: Int) -> Int {
    let optional: Int? = value == 0 ? nil : value
    guard let unwrapped = optional else { return -1 }

    let mode: PatchMode
    switch unwrapped % 3 {
    case 0: mode = .add
    case 1: mode = .multiply
    default: mode = .fallback
    }

    var cursor = 0
    var result = 0
    while cursor < 3 {
        switch mode {
        case .add: result += unwrapped + cursor
        case .multiply: result += unwrapped * (cursor + 1)
        case .fallback: result += cursor
        }
        cursor += 1
    }
    return reachableHelper(result)
}
