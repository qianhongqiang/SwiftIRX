import UIKit

@inline(never)
func hotfixPatch(_ receiver: UIViewController) {
    let label = UILabel(frame: CGRect(x: 0, y: 0, width: 120, height: 30))
    label.text = "hot" + "fix"
    receiver.view.addSubview(label)
}
