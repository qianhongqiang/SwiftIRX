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
        manager.deactivatePatch(for: HotfixDemoABI.addTargetID)
        manager.deactivatePatch(for: HotfixDemoABI.multiplyTargetID)

        let calculator = HotfixableCalculator()
        print("Hotfix demo native: add=\(hotfixableAdd(41)), multiply=\(calculator.multiply(21))")

        let addPatch = HotfixPatch(
            id: "demo.add.v1",
            targetID: HotfixDemoABI.addTargetID,
            signatureID: HotfixDemoABI.addSignatureID,
            entryFunction: "patch",
            ir: """
            define i64 @patch(i64 %value) {
            entry:
              %result = add i64 %value, 10
              ret i64 %result
            }
            """
        )
        let multiplyPatch = HotfixPatch(
            id: "demo.multiply.v1",
            targetID: HotfixDemoABI.multiplyTargetID,
            signatureID: HotfixDemoABI.multiplySignatureID,
            entryFunction: "patch",
            ir: """
            define i64 @patch(ptr %self, i64 %value) {
            entry:
              %result = add i64 %value, 100
              ret i64 %result
            }
            """
        )

        do {
            manager.upsert(addPatch)
            manager.upsert(multiplyPatch)
            try manager.activatePatch(id: addPatch.id)
            try manager.activatePatch(id: multiplyPatch.id)
            defer {
                manager.deactivatePatch(for: addPatch.targetID)
                manager.deactivatePatch(for: multiplyPatch.targetID)
            }
            print("Hotfix demo patched: add=\(hotfixableAdd(41)), multiply=\(calculator.multiply(21))")
        } catch {
            manager.deactivatePatch(for: addPatch.targetID)
            manager.deactivatePatch(for: multiplyPatch.targetID)
            print("Hotfix demo error: \(error)")
        }
    }
}
