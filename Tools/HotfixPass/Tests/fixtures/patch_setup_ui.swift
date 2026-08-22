import UIKit

@inline(never)
func hotfixPatch(_ receiver: UIViewController) {
    let view = UIView(frame: CGRect(x: 100, y: 100, width: 100, height: 100))
    view.backgroundColor = .yellow
    let label = UILabel(frame: CGRect(x: 10, y: 10, width: 80, height: 20))
    label.text = "hello"
    view.addSubview(label)
    receiver.view.addSubview(view)
}
