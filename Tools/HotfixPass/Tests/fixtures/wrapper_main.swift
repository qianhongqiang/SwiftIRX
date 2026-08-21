@main
struct WrapperMain {
    static func main() {
        let result = wrapperAdd(41)
        guard result == 42 else {
            fatalError("unexpected wrapper result: \(result)")
        }
        print(result)
    }
}
