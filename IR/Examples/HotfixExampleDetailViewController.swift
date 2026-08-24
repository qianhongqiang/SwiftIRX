import UIKit

final class HotfixExampleDetailViewController: UIViewController {
    private let example: HotfixExample
    private let resultLabel = UILabel()
    private let runButton = UIButton(type: .system)
    private let previewController = UIViewController()
    private let previewContainer = UIView()

    init(example: HotfixExample) {
        self.example = example
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        configurePage()
    }

    private func configurePage() {
        title = example.title
        view.backgroundColor = .systemGroupedBackground
        navigationItem.leftBarButtonItem = UIBarButtonItem(
            systemItem: .close,
            primaryAction: UIAction { [weak self] _ in
                self?.dismiss(animated: true)
            }
        )

        let scrollView = UIScrollView()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        let stack = UIStackView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .vertical
        stack.spacing = 18

        view.addSubview(scrollView)
        scrollView.addSubview(stack)
        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            stack.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 20),
            stack.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -30),
            stack.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -40),
        ])

        stack.addArrangedSubview(makeOverviewCard())
        stack.addArrangedSubview(makeSectionTitle("发布版本"))
        stack.addArrangedSubview(makeCodeBlock(example.releaseSource))
        stack.addArrangedSubview(makeSectionTitle("Patch 分支"))
        stack.addArrangedSubview(makeCodeBlock(example.patchSource))
        stack.addArrangedSubview(makeSectionTitle("运行结果"))
        stack.addArrangedSubview(makeRunnerCard())

        if example.kind == .uikit {
            configurePreview()
            stack.addArrangedSubview(previewContainer)
            previewContainer.heightAnchor.constraint(equalToConstant: 240).isActive = true
        }
    }

    private func makeOverviewCard() -> UIView {
        let badge = UILabel()
        badge.text = "  \(example.badge)  "
        badge.font = .systemFont(ofSize: 11, weight: .bold)
        badge.textColor = example.tintColor
        badge.backgroundColor = example.tintColor.withAlphaComponent(0.12)
        badge.layer.cornerRadius = 7
        badge.clipsToBounds = true

        let summary = UILabel()
        summary.text = example.summary
        summary.font = .systemFont(ofSize: 18, weight: .semibold)
        summary.numberOfLines = 0

        let behavior = UILabel()
        behavior.text = example.behavior
        behavior.font = .systemFont(ofSize: 14)
        behavior.textColor = .secondaryLabel
        behavior.numberOfLines = 0

        let stack = UIStackView(arrangedSubviews: [badge, summary, behavior])
        stack.axis = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        return makeCard(containing: stack, padding: 16)
    }

    private func makeRunnerCard() -> UIView {
        resultLabel.font = .monospacedSystemFont(ofSize: 14, weight: .medium)
        resultLabel.textColor = .secondaryLabel
        resultLabel.numberOfLines = 0
        resultLabel.text = "点击运行，查看当前 Bundle 中的实现结果。"

        var configuration = UIButton.Configuration.filled()
        configuration.title = example.kind == .uikit ? "加载并执行内置 Patch" : "运行示例"
        configuration.baseBackgroundColor = example.tintColor
        configuration.cornerStyle = .medium
        configuration.buttonSize = .large
        runButton.configuration = configuration
        runButton.addAction(UIAction { [weak self] _ in
            self?.runExample()
        }, for: .touchUpInside)

        let stack = UIStackView(arrangedSubviews: [resultLabel, runButton])
        stack.axis = .vertical
        stack.spacing = 16
        return makeCard(containing: stack, padding: 16)
    }

    private func configurePreview() {
        previewContainer.backgroundColor = .secondarySystemGroupedBackground
        previewContainer.layer.cornerRadius = 16
        previewContainer.layer.borderWidth = 1
        previewContainer.layer.borderColor = UIColor.separator.cgColor
        previewContainer.clipsToBounds = true

        addChild(previewController)
        let preview = previewController.view!
        preview.translatesAutoresizingMaskIntoConstraints = false
        preview.backgroundColor = .systemBackground
        previewContainer.addSubview(preview)
        previewController.didMove(toParent: self)
        NSLayoutConstraint.activate([
            preview.leadingAnchor.constraint(equalTo: previewContainer.leadingAnchor),
            preview.trailingAnchor.constraint(equalTo: previewContainer.trailingAnchor),
            preview.topAnchor.constraint(equalTo: previewContainer.topAnchor),
            preview.bottomAnchor.constraint(equalTo: previewContainer.bottomAnchor),
        ])
    }

    private func runExample() {
        runButton.isEnabled = false
        defer { runButton.isEnabled = true }

        switch example.kind {
        case .integer:
            runScalarPatch(input: 10, baseline: hotfixExampleInteger)
        case .branch:
            runBooleanPatch()
        case .instanceMethod:
            runInstancePatch()
        case .uikit:
            runUIKitPatch()
        case .hostAdapter:
            runScalarPatch(input: 10, baseline: hotfixExampleHostAdapter)
        }
    }

    private func runScalarPatch(input: Int, baseline: (Int) -> Int) {
        do {
            let activation = try activateBundledPatchIfPresent()
            defer {
                if let activation {
                    HotfixManager.shared.deactivate(activation)
                }
            }
            let value = baseline(input)
            resultLabel.text = activation == nil
                ? "发布版：\(input) → \(value)\n未找到 \(example.patchResource).hfpatch"
                : "Patch 已生效：\(input) → \(value)"
            resultLabel.textColor = activation == nil ? .secondaryLabel : .systemGreen
        } catch {
            show(error: error)
        }
    }

    private func runInstancePatch() {
        do {
            let activation = try activateBundledPatchIfPresent()
            defer {
                if let activation {
                    HotfixManager.shared.deactivate(activation)
                }
            }
            let value = HotfixExampleCalculator().multiply(8)
            resultLabel.text = activation == nil
                ? "发布版：8 → \(value)\n未找到 \(example.patchResource).hfpatch"
                : "Patch 已生效：8 → \(value)"
            resultLabel.textColor = activation == nil ? .secondaryLabel : .systemGreen
        } catch {
            show(error: error)
        }
    }

    private func runBooleanPatch() {
        do {
            let activation = try activateBundledPatchIfPresent()
            defer {
                if let activation {
                    HotfixManager.shared.deactivate(activation)
                }
            }
            let value = hotfixExampleBranch(55)
            resultLabel.text = activation == nil
                ? "发布版：55 → \(value)\n未找到 \(example.patchResource).hfpatch"
                : "Patch 已生效：55 → \(value)"
            resultLabel.textColor = activation == nil ? .secondaryLabel : .systemGreen
        } catch {
            show(error: error)
        }
    }

    private func runUIKitPatch() {
        previewController.view.subviews.forEach { $0.removeFromSuperview() }
        do {
            guard let activation = try activateBundledPatchIfPresent() else {
                resultLabel.text = "未找到 \(example.patchResource).hfpatch"
                resultLabel.textColor = .systemOrange
                return
            }
            defer { HotfixManager.shared.deactivate(activation) }

            var frame = HFMakePatchFrame()
            frame.targetID = activation.targetID
            frame.signatureID = activation.signatureID
            frame.flags = HotfixABI.hasReceiverFlag
            frame.receiver.token = UInt64(UInt(bitPattern: Unmanaged
                .passUnretained(previewController)
                .toOpaque()))
            frame.receiver.kind = HotfixABI.objectHandleKind
            frame.receiver.flags = HotfixABI.borrowedHandleFlags

            let status = hf_vm_invoke(&frame)
            guard status == HFStatus(HFStatusApplied) else {
                resultLabel.text = "Patch 执行失败：HFStatus \(status)"
                resultLabel.textColor = .systemRed
                return
            }
            resultLabel.text = "Patch 已执行：ObjectConstruct + Objective-C selector 调用"
            resultLabel.textColor = .systemGreen
        } catch {
            show(error: error)
        }
    }

    private func activateBundledPatchIfPresent() throws -> HotfixBinaryActivation? {
        guard let url = Bundle.main.url(
            forResource: example.patchResource,
            withExtension: "hfpatch"
        ) else {
            return nil
        }
        return try HotfixManager.shared.installAndActivate(
            binaryPatch: Data(contentsOf: url)
        )
    }

    private func show(error: Error) {
        resultLabel.text = "运行失败：\(error)"
        resultLabel.textColor = .systemRed
    }

    private func makeSectionTitle(_ text: String) -> UILabel {
        let label = UILabel()
        label.text = text
        label.font = .systemFont(ofSize: 14, weight: .bold)
        label.textColor = .secondaryLabel
        return label
    }

    private func makeCodeBlock(_ source: String) -> UIView {
        let label = UILabel()
        label.text = source
        label.font = .monospacedSystemFont(ofSize: 12.5, weight: .regular)
        label.textColor = UIColor { traits in
            traits.userInterfaceStyle == .dark
                ? UIColor(red: 0.78, green: 0.87, blue: 1, alpha: 1)
                : UIColor(red: 0.11, green: 0.18, blue: 0.28, alpha: 1)
        }
        label.numberOfLines = 0
        label.setContentCompressionResistancePriority(.required, for: .vertical)

        let card = makeCard(containing: label, padding: 16)
        card.backgroundColor = UIColor { traits in
            traits.userInterfaceStyle == .dark
                ? UIColor(red: 0.08, green: 0.10, blue: 0.14, alpha: 1)
                : UIColor(red: 0.94, green: 0.96, blue: 0.99, alpha: 1)
        }
        return card
    }

    private func makeCard(containing content: UIView, padding: CGFloat) -> UIView {
        let card = UIView()
        card.backgroundColor = .secondarySystemGroupedBackground
        card.layer.cornerRadius = 16
        content.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: padding),
            content.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -padding),
            content.topAnchor.constraint(equalTo: card.topAnchor, constant: padding),
            content.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -padding),
        ])
        return card
    }
}
