import UIKit

final class ViewController: UIViewController {
    private let examples = HotfixExampleCatalog.sections
    private let tableView = UITableView(frame: .zero, style: .insetGrouped)

    override func viewDidLoad() {
        super.viewDidLoad()
        configurePage()
    }

    private func configurePage() {
        view.backgroundColor = .systemGroupedBackground

        tableView.translatesAutoresizingMaskIntoConstraints = false
        tableView.dataSource = self
        tableView.delegate = self
        tableView.rowHeight = 88
        tableView.sectionHeaderTopPadding = 18
        tableView.register(
            HotfixExampleCell.self,
            forCellReuseIdentifier: HotfixExampleCell.reuseIdentifier
        )
        tableView.tableHeaderView = makeHeaderView()
        tableView.tableFooterView = makeFooterView()
        view.addSubview(tableView)

        NSLayoutConstraint.activate([
            tableView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            tableView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            tableView.topAnchor.constraint(equalTo: view.topAnchor),
            tableView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
    }

    private func makeHeaderView() -> UIView {
        let container = UIView(frame: CGRect(x: 0, y: 0, width: 1, height: 156))

        let eyebrow = UILabel()
        eyebrow.translatesAutoresizingMaskIntoConstraints = false
        eyebrow.text = "IR HOTFIX PLAYGROUND"
        eyebrow.font = .systemFont(ofSize: 12, weight: .bold)
        eyebrow.textColor = .systemBlue

        let title = UILabel()
        title.translatesAutoresizingMaskIntoConstraints = false
        title.text = "Patch 语法示例"
        title.font = .systemFont(ofSize: 32, weight: .bold)

        let detail = UILabel()
        detail.translatesAutoresizingMaskIntoConstraints = false
        detail.text = "按语法和宿主调用类型浏览发布版实现、Patch 分支代码，并直接运行示例。"
        detail.font = .systemFont(ofSize: 15)
        detail.textColor = .secondaryLabel
        detail.numberOfLines = 0

        container.addSubview(eyebrow)
        container.addSubview(title)
        container.addSubview(detail)
        NSLayoutConstraint.activate([
            eyebrow.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 20),
            eyebrow.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -20),
            eyebrow.topAnchor.constraint(equalTo: container.topAnchor, constant: 18),
            title.leadingAnchor.constraint(equalTo: eyebrow.leadingAnchor),
            title.trailingAnchor.constraint(equalTo: eyebrow.trailingAnchor),
            title.topAnchor.constraint(equalTo: eyebrow.bottomAnchor, constant: 8),
            detail.leadingAnchor.constraint(equalTo: eyebrow.leadingAnchor),
            detail.trailingAnchor.constraint(equalTo: eyebrow.trailingAnchor),
            detail.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 8),
        ])
        return container
    }

    private func makeFooterView() -> UIView {
        let container = UIView(frame: CGRect(x: 0, y: 0, width: 1, height: 82))
        let label = UILabel()
        label.translatesAutoresizingMaskIntoConstraints = false
        label.text = "示例页会优先加载同名 .hfpatch；没有 Patch 产物时运行发布版实现。"
        label.font = .systemFont(ofSize: 12)
        label.textColor = .tertiaryLabel
        label.textAlignment = .center
        label.numberOfLines = 0
        container.addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 32),
            label.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -32),
            label.topAnchor.constraint(equalTo: container.topAnchor, constant: 16),
        ])
        return container
    }

    // Keep this release target available for the bundled UIKit patch demo.
    @inline(never)
    private func setupUI() {
        let sampleView = UIView(frame: CGRect(x: 100, y: 100, width: 100, height: 100))
        sampleView.backgroundColor = .red
        view.addSubview(sampleView)
    }
}

extension ViewController: UITableViewDataSource, UITableViewDelegate {
    func numberOfSections(in tableView: UITableView) -> Int {
        examples.count
    }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        examples[section].examples.count
    }

    func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        examples[section].title
    }

    func tableView(
        _ tableView: UITableView,
        cellForRowAt indexPath: IndexPath
    ) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(
            withIdentifier: HotfixExampleCell.reuseIdentifier,
            for: indexPath
        ) as! HotfixExampleCell
        cell.configure(with: examples[indexPath.section].examples[indexPath.row])
        return cell
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let example = examples[indexPath.section].examples[indexPath.row]
        let detail = HotfixExampleDetailViewController(example: example)
        let navigation = UINavigationController(rootViewController: detail)
        navigation.modalPresentationStyle = .pageSheet
        present(navigation, animated: true)
    }
}

private final class HotfixExampleCell: UITableViewCell {
    static let reuseIdentifier = "HotfixExampleCell"

    private let iconContainer = UIView()
    private let iconLabel = UILabel()
    private let titleLabel = UILabel()
    private let detailLabel = UILabel()
    private let badgeLabel = UILabel()

    override init(style: UITableViewCell.CellStyle, reuseIdentifier: String?) {
        super.init(style: style, reuseIdentifier: reuseIdentifier)
        configureViews()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    private func configureViews() {
        accessoryType = .disclosureIndicator

        iconContainer.translatesAutoresizingMaskIntoConstraints = false
        iconContainer.layer.cornerRadius = 12
        iconLabel.translatesAutoresizingMaskIntoConstraints = false
        iconLabel.font = .systemFont(ofSize: 20, weight: .semibold)
        iconLabel.textAlignment = .center

        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.font = .systemFont(ofSize: 16, weight: .semibold)
        detailLabel.translatesAutoresizingMaskIntoConstraints = false
        detailLabel.font = .systemFont(ofSize: 13)
        detailLabel.textColor = .secondaryLabel
        detailLabel.numberOfLines = 2

        badgeLabel.translatesAutoresizingMaskIntoConstraints = false
        badgeLabel.font = .systemFont(ofSize: 10, weight: .bold)
        badgeLabel.textColor = .systemBlue
        badgeLabel.backgroundColor = .systemBlue.withAlphaComponent(0.12)
        badgeLabel.layer.cornerRadius = 6
        badgeLabel.clipsToBounds = true
        badgeLabel.textAlignment = .center

        contentView.addSubview(iconContainer)
        iconContainer.addSubview(iconLabel)
        contentView.addSubview(titleLabel)
        contentView.addSubview(detailLabel)
        contentView.addSubview(badgeLabel)

        NSLayoutConstraint.activate([
            iconContainer.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 16),
            iconContainer.centerYAnchor.constraint(equalTo: contentView.centerYAnchor),
            iconContainer.widthAnchor.constraint(equalToConstant: 48),
            iconContainer.heightAnchor.constraint(equalToConstant: 48),
            iconLabel.centerXAnchor.constraint(equalTo: iconContainer.centerXAnchor),
            iconLabel.centerYAnchor.constraint(equalTo: iconContainer.centerYAnchor),

            titleLabel.leadingAnchor.constraint(equalTo: iconContainer.trailingAnchor, constant: 12),
            titleLabel.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 14),
            titleLabel.trailingAnchor.constraint(lessThanOrEqualTo: badgeLabel.leadingAnchor, constant: -8),
            badgeLabel.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -8),
            badgeLabel.centerYAnchor.constraint(equalTo: titleLabel.centerYAnchor),
            badgeLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 50),
            badgeLabel.heightAnchor.constraint(equalToConstant: 22),

            detailLabel.leadingAnchor.constraint(equalTo: titleLabel.leadingAnchor),
            detailLabel.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -8),
            detailLabel.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 5),
            detailLabel.bottomAnchor.constraint(lessThanOrEqualTo: contentView.bottomAnchor, constant: -10),
        ])
    }

    func configure(with example: HotfixExample) {
        iconLabel.text = example.icon
        iconLabel.textColor = example.tintColor
        iconContainer.backgroundColor = example.tintColor.withAlphaComponent(0.12)
        titleLabel.text = example.title
        detailLabel.text = example.summary
        badgeLabel.text = "  \(example.badge)  "
    }
}
