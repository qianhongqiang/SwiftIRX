import UIKit

@inline(never)
func hotfixPatch(_ receiver: UIViewController, _ x: Double) {
    let view = UIView(frame: CGRect(x: x, y: 10, width: 80, height: 40))
    receiver.view.addSubview(view)
}
