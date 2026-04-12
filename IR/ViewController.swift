//
//  ViewController.swift
//  IR
//
//  Created by hongqiang qian on 2026/3/18.
//

import UIKit

class ViewController: UIViewController {
    private let patchPoint = "viewDidLoad.demo"

    override func viewDidLoad() {
        super.viewDidLoad()
        bootstrapHotfixDemo()
        runIRDemo()
//        setupUI()
    }
    
    private func setupUI() {
        let view = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
        view.backgroundColor = .red
        self.view.addSubview(view)
    }

    private func runIRDemo() {
        // Full setupUI function body extracted from ViewController.ll.
        let fallbackIR = """
        define hidden swiftcc void @"$s14ViewControllerAAC7setupUI33_37ACD668159BB52851391EE68C0B8918LLyyF"(ptr swiftself %0) #0 {
        entry:
          %self.debug = alloca ptr, align 8
          call void @llvm.memset.p0.i64(ptr align 8 %self.debug, i8 0, i64 8, i1 false)
          %view.debug = alloca ptr, align 8
          call void @llvm.memset.p0.i64(ptr align 8 %view.debug, i8 0, i64 8, i1 false)
          store ptr %0, ptr %self.debug, align 8
          %1 = call swiftcc %swift.metadata_response @"$sSo6UIViewCMa"(i64 0)
          %2 = extractvalue %swift.metadata_response %1, 0
          %3 = call swiftcc ptr @"$sSo6UIViewC5frameABSo6CGRectV_tcfC"(double 0.000000e+00, double 0.000000e+00, double 1.000000e+02, double 1.000000e+02, ptr swiftself %2)
          store ptr %3, ptr %view.debug, align 8
          %4 = load ptr, ptr @"OBJC_CLASS_REF_$_UIColor", align 8
          %5 = call ptr @objc_opt_self(ptr %4)
          %6 = load ptr, ptr @"\01L_selector(redColor)", align 8
          %7 = call ptr @objc_msgSend(ptr %5, ptr %6)
          call void asm sideeffect "mov\09fp, fp\09\09// marker for objc_retainAutoreleaseReturnValue", ""()
          %8 = call ptr @llvm.objc.retainAutoreleasedReturnValue(ptr %7)
          %9 = ptrtoint ptr %8 to i64
          %10 = load ptr, ptr @"\01L_selector(setBackgroundColor:)", align 8
          %11 = inttoptr i64 %9 to ptr
          call void @objc_msgSend(ptr %3, ptr %10, ptr %11)
          %12 = inttoptr i64 %9 to ptr
          call void @llvm.objc.release(ptr %12)
          %13 = call ptr @llvm.objc.retain(ptr %0)
          %14 = load ptr, ptr @"\01L_selector(view)", align 8
          %15 = call ptr @objc_msgSend(ptr %0, ptr %14)
          call void asm sideeffect "mov\09fp, fp\09\09// marker for objc_retainAutoreleaseReturnValue", ""()
          %16 = call ptr @llvm.objc.retainAutoreleasedReturnValue(ptr %15)
          %17 = ptrtoint ptr %16 to i64
          call void @llvm.objc.release(ptr %0)
          %18 = icmp eq i64 %17, 0
          br i1 %18, label %21, label %19

        19:
          %20 = inttoptr i64 %17 to ptr
          br label %22

        21:
          call swiftcc void @"$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_A2HSus6UInt32VtF"(i64 ptrtoint (ptr @".str.11.Fatal error" to i64), i64 11, i8 2, i64 ptrtoint (ptr @".str.68.Unexpectedly found nil while implicitly unwrapping an Optional value" to i64), i64 68, i8 2, i64 ptrtoint (ptr @".str.35.ViewController/ViewController.swift" to i64), i64 35, i8 2, i64 21, i32 0)
          unreachable

        22:
          %23 = phi ptr [ %20, %19 ]
          %24 = load ptr, ptr @"\01L_selector(addSubview:)", align 8
          call void @objc_msgSend(ptr %23, ptr %24, ptr %3)
          call void @llvm.objc.release(ptr %23)
          call void @llvm.objc.release(ptr %3)
          ret void
        }

        define hidden swiftcc %swift.metadata_response @"$s14ViewControllerAACMa"(i64 %0) {
        entry:
          %1 = call ptr @objc_opt_self(ptr getelementptr inbounds (<{ ptr, ptr, ptr, i64, ptr, ptr, ptr, ptr, i32, i32, i32, i16, i16, i32, i32, ptr, ptr, ptr, ptr }>, ptr @"$s14ViewControllerAACMf", i32 0, i32 3))
          %2 = insertvalue %swift.metadata_response undef, ptr %1, 0
          %3 = insertvalue %swift.metadata_response %2, i64 0, 1
          ret %swift.metadata_response %3
        }

        define hidden swiftcc void @"$s14ViewControllerAAC11viewDidLoadyyF"(ptr swiftself %0) {
        entry:
          %self.debug = alloca ptr, align 8
          call void @llvm.memset.p0.i64(ptr align 8 %self.debug, i8 0, i64 8, i1 false)
          %objc_super = alloca %objc_super, align 8
          store ptr %0, ptr %self.debug, align 8
          %1 = call ptr @llvm.objc.retain(ptr %0)
          %2 = call swiftcc %swift.metadata_response @"$s14ViewControllerAACMa"(i64 0)
          %3 = extractvalue %swift.metadata_response %2, 0
          %4 = getelementptr inbounds %objc_super, ptr %objc_super, i32 0, i32 0
          store ptr %0, ptr %4, align 8
          %5 = getelementptr inbounds %objc_super, ptr %objc_super, i32 0, i32 1
          store ptr %3, ptr %5, align 8
          %6 = load ptr, ptr @"\01L_selector(viewDidLoad)", align 8
          call void @objc_msgSendSuper2(ptr %objc_super, ptr %6)
          call void @llvm.objc.release(ptr %0)
          call swiftcc void @"$s14ViewControllerAAC9runIRDemo33_37ACD668159BB52851391EE68C0B8918LLyyF"(ptr swiftself %0)
          call swiftcc void @"$s14ViewControllerAAC7setupUI33_37ACD668159BB52851391EE68C0B8918LLyyF"(ptr swiftself %0)
          ret void
        }

        define i32 @main() {
        entry:
          call swiftcc void @"$s14ViewControllerAAC11viewDidLoadyyF"(ptr null)
          ret i32 42
        }
        """

        do {
            let host = LLVMHostContext(rootViewController: self)
            let executor = HotfixExecutor()
            let execution = try executor.runMain(
                patchPoint: patchPoint,
                fallbackIR: fallbackIR,
                host: host
            )
            if let patchID = execution.patchID {
                print("LLVM IR interpreted result: \(execution.result) (hotfix: \(patchID))")
            } else {
                print("LLVM IR interpreted result: \(execution.result)")
            }
        } catch {
            print("LLVM IR interpreter error: \(error)")
        }
    }

    private func bootstrapHotfixDemo() {
        // Demonstration patch: keeps behavior unchanged, but proves patch pipeline works.
        let patch = HotfixPatch(
            id: "demo.v1",
            patchPoint: patchPoint,
            ir: """
            define i32 @main() {
            entry:
              ret i32 42
            }
            """
        )
        let manager = HotfixManager.shared
        manager.upsert(patch)
        // Keep fallback IR as default behavior; uncomment to force this demo hotfix.
        // try? manager.activatePatch(id: patch.id)
    }

}

