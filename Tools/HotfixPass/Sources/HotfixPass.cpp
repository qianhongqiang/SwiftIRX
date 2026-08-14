#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {
class HotfixPass : public PassInfoMixin<HotfixPass> {
public:
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    if (module.getGlobalVariable("hotfix_pass_loaded") == nullptr) {
      Constant *marker = ConstantDataArray::getString(
          module.getContext(), "hotfix-pass-loaded", true);
      new GlobalVariable(module, marker->getType(), true,
                         GlobalValue::ExternalLinkage, marker,
                         "hotfix_pass_loaded");
    }

    return PreservedAnalyses::none();
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "HotfixPass", "swift-6.2.4-llvm-19.1.5",
          [](PassBuilder &passBuilder) {
            passBuilder.registerPipelineStartEPCallback(
                [](ModulePassManager &modulePassManager, OptimizationLevel) {
                  modulePassManager.addPass(HotfixPass());
                });
          }};
}
