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
        setupUI()
        runHotfixDemo()
    }

    private func setupUI() {
        let view = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
        view.backgroundColor = .red
        self.view.addSubview(view)
    }

    private func runHotfixDemo() {
        let manager = HotfixManager.shared
        let calculator = HotfixableCalculator()
        print("Hotfix demo native: add=\(hotfixableAdd(41)), multiply=\(calculator.multiply(21))")

        var activations: [HotfixActivation] = []
        do {
            for resourceName in ["HotfixAdd", "HotfixMultiply"] {
                guard let url = Bundle.main.url(forResource: resourceName, withExtension: "irpatch") else {
                    throw CocoaError(.fileNoSuchFile)
                }
                let textPatch = try String(contentsOf: url, encoding: .utf8)
                activations.append(try manager.installAndActivate(textPatch: textPatch))
            }
            defer {
                for activation in activations {
                    manager.deactivate(activation)
                }
            }
            print("Hotfix demo patched: add=\(hotfixableAdd(41)), multiply=\(calculator.multiply(21))")
        } catch {
            for activation in activations {
                manager.deactivate(activation)
            }
            print("Hotfix demo error: \(error)")
        }
    }
}
