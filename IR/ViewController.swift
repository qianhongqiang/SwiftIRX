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

    private func runHotfixDemo() -> [HotfixActivation] {
        let manager = HotfixManager.shared
        let calculator = HotfixableCalculator()
        print("Hotfix demo native: add=\(hotfixableAdd(41)), multiply=\(calculator.multiply(21))")

        var activations: [HotfixActivation] = []
        do {
            for resourceName in ["HotfixAdd", "HotfixMultiply", "HotfixSetupUI"] {
                guard let url = Bundle.main.url(forResource: resourceName, withExtension: "irpatch") else {
                    throw CocoaError(.fileNoSuchFile)
                }
                let textPatch = try String(contentsOf: url, encoding: .utf8)
                activations.append(try manager.installAndActivate(textPatch: textPatch))
            }
            print("Hotfix demo patched: add=\(hotfixableAdd(41)), multiply=\(calculator.multiply(21))")
            return activations
        } catch {
            for activation in activations {
                manager.deactivate(activation)
            }
            print("Hotfix demo error: \(error)")
            return []
        }
    }
}
