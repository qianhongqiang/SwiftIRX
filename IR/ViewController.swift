//
//  ViewController.swift
//  IR
//
//  Created by hongqiang qian on 2026/3/18.
//

import UIKit

class ViewController: UIViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        let activations = runHotfixDemo()
        defer {
            for activation in activations {
                HotfixManager.shared.deactivate(activation)
            }
        }
        setupUI()
    }

    @inline(never)
    private func setupUI() {
        let view = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
        view.backgroundColor = .red
        self.view.addSubview(view)
    }

    private func runHotfixDemo() -> [HotfixBinaryActivation] {
        let manager = HotfixManager.shared
        do {
            guard let url = Bundle.main.url(
                forResource: "HotfixSetupUI",
                withExtension: "hfpatch"
            ) else {
                throw CocoaError(.fileNoSuchFile)
            }
            let activation = try manager.installAndActivate(
                binaryPatch: Data(contentsOf: url)
            )
            return [activation]
        } catch {
            print("Hotfix demo error: \(error)")
            return []
        }
    }
}
