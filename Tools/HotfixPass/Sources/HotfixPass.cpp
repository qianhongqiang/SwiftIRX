#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdint>
#include <string>

using namespace llvm;

namespace {
constexpr StringLiteral InstrumentedModuleFlag = "ir.hotfix.instrumented";
constexpr StringLiteral OriginalSuffix = ".hotfix_original";

uint64_t fnv1a64(StringRef text) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : text.bytes()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool isScalar(Type *type) {
  return type->isIntegerTy(64) || type->isIntegerTy(1);
}

StringRef scalarName(Type *type) {
  return type->isIntegerTy(64) ? "i64" : "i1";
}

std::string canonicalSignature(const Function &function) {
  std::string signature = "return=";
  signature += function.getReturnType()->isVoidTy()
                   ? "void"
                   : scalarName(function.getReturnType());
  signature += ";arguments=";

  bool first = true;
  for (const Argument &argument : function.args()) {
    if (!first)
      signature += ',';
    first = false;
    signature += scalarName(argument.getType());
  }

  signature += ";receiver=0";
  return signature;
}

bool isEligible(const Function &function) {
  if (function.empty() || function.isIntrinsic() || function.isVarArg() ||
      function.getCallingConv() != CallingConv::Swift)
    return false;

  StringRef name = function.getName();
  if (name.starts_with("ir_hotfix_") || name.ends_with(OriginalSuffix))
    return false;

  Type *returnType = function.getReturnType();
  if (!returnType->isVoidTy() && !isScalar(returnType))
    return false;

  for (const Argument &argument : function.args()) {
    if (!isScalar(argument.getType()))
      return false;
  }
  return true;
}

void addLoadMarker(Module &module) {
  if (module.getGlobalVariable("hotfix_pass_loaded") != nullptr)
    return;

  Constant *marker = ConstantDataArray::getString(module.getContext(),
                                                  "hotfix-pass-loaded", true);
  new GlobalVariable(module, marker->getType(), true,
                     GlobalValue::ExternalLinkage, marker,
                     "hotfix_pass_loaded");
}

FunctionCallee getRuntimeInvoke(Module &module) {
  LLVMContext &context = module.getContext();
  Type *i1 = Type::getInt1Ty(context);
  Type *i32 = Type::getInt32Ty(context);
  Type *i64 = Type::getInt64Ty(context);
  Type *pointer = PointerType::getUnqual(context);
  FunctionType *type = FunctionType::get(
      i1, {i64, i64, pointer, pointer, i32, pointer, pointer}, false);
  return module.getOrInsertFunction("ir_hotfix_invoke", type);
}

Function *cloneOriginal(Function &function) {
  GlobalValue::LinkageTypes originalLinkage = function.getLinkage();
  bool originalDSOLocal = function.isDSOLocal();
  ValueToValueMapTy valueMap;
  Function *clone = CloneFunction(&function, valueMap);
  function.setLinkage(originalLinkage);
  function.setDSOLocal(originalDSOLocal);

  clone->setName(function.getName() + OriginalSuffix);
  clone->setLinkage(GlobalValue::PrivateLinkage);
  clone->setDSOLocal(true);
  clone->setVisibility(GlobalValue::DefaultVisibility);
  clone->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  clone->setComdat(nullptr);

  for (BasicBlock &block : *clone) {
    for (Instruction &instruction : block) {
      for (Use &operand : instruction.operands()) {
        if (operand.get() == &function)
          operand.set(clone);
      }
    }
  }
  return clone;
}

void prepareTrampolineAttributes(Function &function) {
  constexpr Attribute::AttrKind invalidBehavior[] = {
      Attribute::AllocKind,    Attribute::AllocSize,  Attribute::Memory,
      Attribute::MustProgress, Attribute::Naked,      Attribute::NoCallback,
      Attribute::NoFree,       Attribute::NoRecurse,  Attribute::NoReturn,
      Attribute::NoSync,       Attribute::NoUnwind,   Attribute::ReturnsTwice,
      Attribute::Speculatable, Attribute::WillReturn,
  };
  for (Attribute::AttrKind kind : invalidBehavior)
    function.removeFnAttr(kind);

  function.removeRetAttr(Attribute::Range);
  for (Argument &argument : function.args())
    argument.removeAttr(Attribute::Returned);

  function.removeFnAttr(Attribute::AlwaysInline);
  function.addFnAttr(Attribute::NoInline);
}

Function *instrument(Function &function, FunctionCallee runtimeInvoke) {
  Module &module = *function.getParent();
  LLVMContext &context = module.getContext();
  GlobalValue::LinkageTypes originalLinkage = function.getLinkage();
  bool originalDSOLocal = function.isDSOLocal();
  Function *clone = cloneOriginal(function);
  clone->removeFnAttr(Attribute::AlwaysInline);
  clone->addFnAttr(Attribute::NoInline);
  const AttributeList fallbackAttributes = clone->getAttributes();

  function.deleteBody();
  function.setLinkage(originalLinkage);
  function.setDSOLocal(originalDSOLocal);
  prepareTrampolineAttributes(function);

  BasicBlock *entry = BasicBlock::Create(context, "entry", &function);
  BasicBlock *patched =
      BasicBlock::Create(context, "hotfix.patched", &function);
  BasicBlock *fallback =
      BasicBlock::Create(context, "hotfix.fallback", &function);
  IRBuilder<> builder(entry);

  Type *i8 = builder.getInt8Ty();
  Type *i32 = builder.getInt32Ty();
  Type *i64 = builder.getInt64Ty();
  PointerType *pointer = builder.getPtrTy();
  const unsigned argumentCount = function.arg_size();

  Value *kindsPointer = ConstantPointerNull::get(pointer);
  Value *bitsPointer = ConstantPointerNull::get(pointer);
  if (argumentCount != 0) {
    ArrayType *kindsType = ArrayType::get(i8, argumentCount);
    ArrayType *bitsType = ArrayType::get(i64, argumentCount);
    AllocaInst *kinds =
        builder.CreateAlloca(kindsType, nullptr, "hotfix.argument.kinds");
    AllocaInst *bits =
        builder.CreateAlloca(bitsType, nullptr, "hotfix.argument.bits");
    kinds->setAlignment(Align(1));
    bits->setAlignment(Align(8));
    kindsPointer = kinds;
    bitsPointer = bits;

    unsigned index = 0;
    for (Argument &argument : function.args()) {
      Value *indices[] = {builder.getInt32(0), builder.getInt32(index)};
      Value *kindSlot = builder.CreateInBoundsGEP(kindsType, kinds, indices,
                                                  "hotfix.argument.kind");
      Value *bitsSlot = builder.CreateInBoundsGEP(bitsType, bits, indices,
                                                  "hotfix.argument.bits.slot");

      uint8_t kind = argument.getType()->isIntegerTy(64) ? 1 : 2;
      StoreInst *kindStore =
          builder.CreateStore(builder.getInt8(kind), kindSlot);
      kindStore->setAlignment(Align(1));

      Value *encoded = &argument;
      if (argument.getType()->isIntegerTy(1))
        encoded =
            builder.CreateZExt(&argument, i64, "hotfix.argument.bits.value");
      StoreInst *bitsStore = builder.CreateStore(encoded, bitsSlot);
      bitsStore->setAlignment(Align(8));
      ++index;
    }
  }

  Value *resultPointer = ConstantPointerNull::get(pointer);
  AllocaInst *resultBits = nullptr;
  if (!function.getReturnType()->isVoidTy()) {
    resultBits = builder.CreateAlloca(i64, nullptr, "hotfix.result.bits");
    resultBits->setAlignment(Align(8));
    resultPointer = resultBits;
  }

  uint64_t targetID = fnv1a64(function.getName());
  uint64_t signatureID = fnv1a64(canonicalSignature(function));
  CallInst *applied = builder.CreateCall(
      runtimeInvoke,
      {ConstantInt::get(i64, targetID), ConstantInt::get(i64, signatureID),
       kindsPointer, bitsPointer, ConstantInt::get(i32, argumentCount),
       ConstantPointerNull::get(pointer), resultPointer},
      "hotfix.applied");
  builder.CreateCondBr(applied, patched, fallback);

  builder.SetInsertPoint(patched);
  if (function.getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    LoadInst *loaded =
        builder.CreateLoad(i64, resultBits, "hotfix.result.value");
    loaded->setAlignment(Align(8));
    Value *result = loaded;
    if (function.getReturnType()->isIntegerTy(1))
      result = builder.CreateTrunc(loaded, builder.getInt1Ty(),
                                   "hotfix.result.boolean");
    builder.CreateRet(result);
  }

  builder.SetInsertPoint(fallback);
  SmallVector<Value *> arguments;
  for (Argument &argument : function.args())
    arguments.push_back(&argument);
  CallInst *native =
      function.getReturnType()->isVoidTy()
          ? builder.CreateCall(clone, arguments)
          : builder.CreateCall(clone, arguments, "hotfix.native");
  native->setCallingConv(function.getCallingConv());
  native->setAttributes(fallbackAttributes);
  if (function.getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(native);
  }
  return clone;
}

class HotfixPass : public PassInfoMixin<HotfixPass> {
public:
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    if (module.getModuleFlag(InstrumentedModuleFlag) != nullptr)
      return PreservedAnalyses::all();

    module.addModuleFlag(Module::Warning, InstrumentedModuleFlag, 1);
    addLoadMarker(module);

    SmallVector<Function *> eligibleFunctions;
    for (Function &function : module) {
      std::string cloneName = (function.getName() + OriginalSuffix).str();
      if (isEligible(function) && module.getFunction(cloneName) == nullptr)
        eligibleFunctions.push_back(&function);
    }

    if (!eligibleFunctions.empty()) {
      FunctionCallee runtimeInvoke = getRuntimeInvoke(module);
      SmallVector<GlobalValue *> retainedFunctions;
      retainedFunctions.reserve(eligibleFunctions.size() * 2);
      for (Function *function : eligibleFunctions) {
        Function *clone = instrument(*function, runtimeInvoke);
        retainedFunctions.push_back(function);
        retainedFunctions.push_back(clone);
      }
      appendToCompilerUsed(module, retainedFunctions);
    }

    return PreservedAnalyses::none();
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "HotfixPass", "homebrew-llvm-19.1.7",
          [](PassBuilder &passBuilder) {
            passBuilder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &modulePassManager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "hotfix-instrument")
                    return false;
                  modulePassManager.addPass(HotfixPass());
                  return true;
                });
            passBuilder.registerPipelineStartEPCallback(
                [](ModulePassManager &modulePassManager, OptimizationLevel) {
                  modulePassManager.addPass(HotfixPass());
                });
          }};
}
