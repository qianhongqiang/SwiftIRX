import UIKit

enum HotfixExampleKind: String, Sendable {
    case integer
    case branch
    case instanceMethod
    case uikit
    case hostAdapter
    case cFunction
    case cxxMethod
}

struct HotfixExample: Sendable {
    let kind: HotfixExampleKind
    let title: String
    let summary: String
    let icon: String
    let badge: String
    let tintName: String
    let patchResource: String
    let releaseSource: String
    let patchSource: String
    let behavior: String

    @MainActor
    var tintColor: UIColor {
        switch tintName {
        case "orange": .systemOrange
        case "green": .systemGreen
        case "purple": .systemPurple
        case "pink": .systemPink
        default: .systemBlue
        }
    }
}

struct HotfixExampleSection: Sendable {
    let title: String
    let examples: [HotfixExample]
}

enum HotfixExampleCatalog {
    static let sections: [HotfixExampleSection] = [
        HotfixExampleSection(
            title: "基础语法",
            examples: [
                HotfixExample(
                    kind: .integer,
                    title: "整数运算",
                    summary: "Int 参数、返回值和算术指令",
                    icon: "+1",
                    badge: "I64",
                    tintName: "blue",
                    patchResource: "HotfixInteger",
                    releaseSource: """
                    @inline(never)
                    func hotfixExampleInteger(_ value: Int) -> Int {
                        value + 1
                    }
                    """,
                    patchSource: """
                    @HotfixPatch
                    @inline(never)
                    func hotfixExampleInteger(_ value: Int) -> Int {
                        value + 100
                    }
                    """,
                    behavior: "输入 10：发布版返回 11，Patch 后返回 110。"
                ),
                HotfixExample(
                    kind: .branch,
                    title: "比较与布尔值",
                    summary: "整数比较和 Bool 返回值",
                    icon: "if",
                    badge: "BOOL",
                    tintName: "orange",
                    patchResource: "HotfixBranch",
                    releaseSource: """
                    @inline(never)
                    func hotfixExampleBranch(_ score: Int) -> Bool {
                        score > 59
                    }
                    """,
                    patchSource: """
                    @HotfixPatch
                    @inline(never)
                    func hotfixExampleBranch(_ score: Int) -> Bool {
                        score > 49
                    }
                    """,
                    behavior: "输入 55：发布版返回 false，Patch 将及格线调整后返回 true。"
                ),
            ]
        ),
        HotfixExampleSection(
            title: "对象与宿主调用",
            examples: [
                HotfixExample(
                    kind: .instanceMethod,
                    title: "Swift 实例方法",
                    summary: "带 receiver 的 class 方法调用",
                    icon: "ƒ",
                    badge: "RECEIVER",
                    tintName: "green",
                    patchResource: "HotfixInstance",
                    releaseSource: """
                    final class HotfixExampleCalculator {
                        @inline(never)
                        func multiply(_ value: Int) -> Int {
                            value * 2
                        }
                    }
                    """,
                    patchSource: """
                    final class HotfixExampleCalculator {
                        @HotfixPatch
                        @inline(never)
                        func multiply(_ value: Int) -> Int {
                            value * 3
                        }
                    }
                    """,
                    behavior: "输入 8：发布版返回 16，Patch 后返回 24。"
                ),
                HotfixExample(
                    kind: .uikit,
                    title: "UIKit / Objective-C",
                    summary: "构造对象、属性赋值和 selector 调用",
                    icon: "UI",
                    badge: "OBJC",
                    tintName: "purple",
                    patchResource: "HotfixSetupUI",
                    releaseSource: """
                    private func setupUI() {
                        let box = UIView(frame: CGRect(
                            x: 100, y: 100, width: 100, height: 100
                        ))
                        box.backgroundColor = .red
                        view.addSubview(box)
                    }
                    """,
                    patchSource: """
                    @HotfixPatch
                    private func setupUI() {
                        let box = UIView(frame: CGRect(
                            x: 100, y: 100, width: 100, height: 100
                        ))
                        box.backgroundColor = .yellow
                        let label = UILabel(frame: CGRect(
                            x: 10, y: 10, width: 80, height: 20
                        ))
                        label.text = "hello"
                        box.addSubview(label)
                        view.addSubview(box)
                    }
                    """,
                    behavior: "内置 Patch 会通过 Objective-C bridge 创建 UIView、UILabel 并调用 addSubview。"
                ),
                HotfixExample(
                    kind: .hostAdapter,
                    title: "Swift Host Adapter",
                    summary: "HFIR 调用已发布 App 中的原生函数",
                    icon: "↗",
                    badge: "HOST",
                    tintName: "pink",
                    patchResource: "HotfixHostAdapter",
                    releaseSource: """
                    @inline(never)
                    func hotfixExampleHostAdapter(_ value: Int) -> Int {
                        value
                    }
                    """,
                    patchSource: """
                    @HotfixPatch
                    @inline(never)
                    func hotfixExampleHostAdapter(_ value: Int) -> Int {
                        hotfixableAdd(value) * 2
                    }
                    """,
                    behavior: "输入 10：Patch 通过生成的 Swift Adapter 调用 hotfixableAdd，再返回 22。"
                ),
            ]
        ),
        HotfixExampleSection(
            title: "C / C++",
            examples: [
                HotfixExample(
                    kind: .cFunction,
                    title: "C 自由函数",
                    summary: "C ABI、标量参数和无 receiver 调用",
                    icon: "C",
                    badge: "C ABI",
                    tintName: "orange",
                    patchResource: "HotfixC",
                    releaseSource: """
                    int64_t hotfix_example_c_add(int64_t value) {
                        // 通用 trampoline，未命中 Patch 时回退。
                        return value + 2;
                    }
                    """,
                    patchSource: """
                    int64_t hotfixPatch(int64_t value) {
                        return value + 200;
                    }
                    """,
                    behavior: "输入 10：发布版返回 12，Clang Patch 构建后返回 210。"
                ),
                HotfixExample(
                    kind: .cxxMethod,
                    title: "C++ 实例方法",
                    summary: "非虚方法、native receiver 和局部 helper",
                    icon: "C++",
                    badge: "NATIVE",
                    tintName: "green",
                    patchResource: "HotfixCXX",
                    releaseSource: """
                    class HFCalculator final {
                    public:
                        long long multiply(long long value) {
                            // this 作为 native scoped handle 传入 VM。
                            return value * 2;
                        }
                    };
                    """,
                    patchSource: """
                    __attribute__((noinline))
                    static long long patchedMultiply(long long value) {
                        return value * 5;
                    }

                    long long hotfixPatch(void *receiver, long long value) {
                        (void)receiver;
                        return patchedMultiply(value);
                    }
                    """,
                    behavior: "输入 8：发布版返回 16，Patch 提取局部 helper 后返回 40。"
                ),
            ]
        ),
    ]
}

@inline(never)
nonisolated func hotfixExampleInteger(_ value: Int) -> Int {
    value + 1
}

@inline(never)
nonisolated func hotfixExampleBranch(_ score: Int) -> Bool {
    score > 59
}

nonisolated final class HotfixExampleCalculator {
    @inline(never)
    func multiply(_ value: Int) -> Int {
        value * 2
    }
}

@inline(never)
nonisolated func hotfixExampleHostAdapter(_ value: Int) -> Int {
    value
}
