import UIKit

func hotfixPatch(_ receiver: UIViewController) {
    let box = UIView(frame: CGRect(x: 100, y: 100, width: 100, height: 100))
    box.backgroundColor = .yellow
    let label = UILabel(frame: CGRect(x: 10, y: 10, width: 80, height: 20))
    label.text = "hello"
    box.addSubview(label)
    receiver.view.addSubview(box)
}
