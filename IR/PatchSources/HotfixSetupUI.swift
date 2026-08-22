import UIKit

func hotfixPatch(_ receiver: UIViewController) {
    let view = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
    view.backgroundColor = .red
    receiver.view.addSubview(view)
}
