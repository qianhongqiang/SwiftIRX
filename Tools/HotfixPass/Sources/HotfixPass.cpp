#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "IRHotfixABI.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using namespace llvm;

namespace {
constexpr StringLiteral InstrumentedModuleFlag = "ir.hotfix.instrumented";
constexpr StringLiteral OriginalSuffix = ".hotfix_original";
constexpr StringLiteral DescriptorSection = "__DATA,__hotfix";
constexpr StringLiteral ReceiverManifestEnvironment =
    "IR_HOTFIX_CLASS_RECEIVER_MANIFEST";
constexpr unsigned MaximumScalarArgumentCount =
    HF_MAX_SCALAR_ARGUMENT_COUNT;

enum class FunctionClassification { NotCandidate, Eligible, Skipped };

struct FunctionShape {
  FunctionClassification classification = FunctionClassification::NotCandidate;
  StringRef skipReason;
  Argument *receiver = nullptr;
  SmallVector<Argument *> scalarArguments;
};

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

std::string canonicalSignature(const Function &function,
                               const FunctionShape &shape) {
  std::string signature = "return=";
  signature += function.getReturnType()->isVoidTy()
                   ? "void"
                   : scalarName(function.getReturnType());
  signature += ";arguments=";

  bool first = true;
  for (const Argument *argument : shape.scalarArguments) {
    if (!first)
      signature += ',';
    first = false;
    signature += scalarName(argument->getType());
  }

  signature += shape.receiver == nullptr ? ";receiver=0" : ";receiver=1";
  return signature;
}

StringSet<> loadClassReceiverManifest() {
  StringSet<> receiverSymbols;
  const char *manifestPath = std::getenv(ReceiverManifestEnvironment.data());
  if (manifestPath == nullptr)
    return receiverSymbols;

  auto manifest = MemoryBuffer::getFile(manifestPath);
  if (!manifest)
    report_fatal_error(Twine("[HotfixPass] error: cannot read class receiver "
                             "manifest '") +
                           manifestPath + "': " + manifest.getError().message(),
                       false);

  SmallVector<StringRef> lines;
  manifest.get()->getBuffer().split(lines, '\n', -1, false);
  for (StringRef line : lines) {
    line.consume_back("\r");
    receiverSymbols.insert(line);
  }
  return receiverSymbols;
}

FunctionShape classifyFunction(Function &function,
                               const StringSet<> &receiverSymbols) {
  FunctionShape shape;
  if (function.empty() || function.isIntrinsic() ||
      function.getCallingConv() != CallingConv::Swift)
    return shape;

  StringRef name = function.getName();
  if (name.starts_with("ir_hotfix_") || name.starts_with("hf_vm_") ||
      name.ends_with(OriginalSuffix))
    return shape;

  shape.classification = FunctionClassification::Skipped;
  if (function.isVarArg()) {
    shape.skipReason = "variadic function";
    return shape;
  }

  Type *returnType = function.getReturnType();
  if (!returnType->isVoidTy() && !isScalar(returnType)) {
    shape.skipReason = "unsupported return type";
    return shape;
  }

  for (Argument &argument : function.args()) {
    if (argument.hasAttribute(Attribute::SwiftSelf)) {
      if (!argument.getType()->isPointerTy()) {
        shape.skipReason = "malformed swiftself receiver argument";
        return shape;
      }
      if (shape.receiver != nullptr) {
        shape.skipReason = "multiple swiftself receiver arguments";
        return shape;
      }
      shape.receiver = &argument;
      continue;
    }

    if (argument.getType()->isPointerTy()) {
      shape.skipReason = "unsupported non-receiver pointer argument";
      return shape;
    }
    if (!isScalar(argument.getType())) {
      shape.skipReason = "unsupported argument type";
      return shape;
    }
    shape.scalarArguments.push_back(&argument);
    if (shape.scalarArguments.size() > MaximumScalarArgumentCount) {
      shape.skipReason = "too many scalar arguments";
      return shape;
    }
  }

  if (shape.receiver != nullptr &&
      !receiverSymbols.contains(function.getName())) {
    shape.skipReason = "unverified swiftself receiver";
    return shape;
  }

  shape.classification = FunctionClassification::Eligible;
  return shape;
}

void addLoadMarker(Module &module) {
  if (module.getGlobalVariable("hotfix_pass_loaded", true) != nullptr)
    return;

  Constant *marker = ConstantDataArray::getString(module.getContext(),
                                                  "hotfix-pass-loaded", true);
  new GlobalVariable(module, marker->getType(), true,
                     GlobalValue::PrivateLinkage, marker, "hotfix_pass_loaded");
}

StructType *getHFValueType(LLVMContext &context) {
  SmallVector<Type *> fields = {
      Type::getInt32Ty(context), Type::getInt32Ty(context),
      Type::getInt64Ty(context), PointerType::getUnqual(context),
      Type::getInt64Ty(context)};
  StructType *type = StructType::getTypeByName(context, "struct.HFValue");
  if (type == nullptr)
    return StructType::create(context, fields, "struct.HFValue");
  if (type->isOpaque()) {
    type->setBody(fields);
    return type;
  }
  if (!type->isLayoutIdentical(StructType::get(context, fields)))
    report_fatal_error("[HotfixPass] incompatible struct.HFValue layout",
                       false);
  return type;
}

StructType *getHFHandleType(LLVMContext &context) {
  SmallVector<Type *> fields = {
      Type::getInt64Ty(context), Type::getInt32Ty(context),
      Type::getInt16Ty(context), Type::getInt16Ty(context)};
  StructType *type = StructType::getTypeByName(context, "struct.HFHandle");
  if (type == nullptr)
    return StructType::create(context, fields, "struct.HFHandle");
  if (type->isOpaque()) {
    type->setBody(fields);
    return type;
  }
  if (!type->isLayoutIdentical(StructType::get(context, fields)))
    report_fatal_error("[HotfixPass] incompatible struct.HFHandle layout",
                       false);
  return type;
}

StructType *getHFPatchFrameType(LLVMContext &context) {
  StructType *handle = getHFHandleType(context);
  StructType *value = getHFValueType(context);
  SmallVector<Type *> fields = {
      Type::getInt32Ty(context), Type::getInt32Ty(context),
      Type::getInt64Ty(context), Type::getInt64Ty(context),
      PointerType::getUnqual(context), Type::getInt32Ty(context),
      Type::getInt32Ty(context), handle, value,
      Type::getInt32Ty(context), Type::getInt32Ty(context)};
  StructType *type =
      StructType::getTypeByName(context, "struct.HFPatchFrame");
  if (type == nullptr)
    return StructType::create(context, fields, "struct.HFPatchFrame");
  if (type->isOpaque()) {
    type->setBody(fields);
    return type;
  }
  if (!type->isLayoutIdentical(StructType::get(context, fields)))
    report_fatal_error("[HotfixPass] incompatible struct.HFPatchFrame layout",
                       false);
  return type;
}

StructType *getHFDescriptorType(LLVMContext &context) {
  SmallVector<Type *> fields = {
      Type::getInt32Ty(context), Type::getInt32Ty(context),
      Type::getInt64Ty(context), Type::getInt64Ty(context),
      Type::getInt32Ty(context), Type::getInt32Ty(context),
      Type::getInt32Ty(context), Type::getInt32Ty(context),
      PointerType::getUnqual(context), PointerType::getUnqual(context)};
  StructType *type =
      StructType::getTypeByName(context, "struct.HFDescriptor");
  if (type == nullptr)
    return StructType::create(context, fields, "struct.HFDescriptor");
  if (type->isOpaque()) {
    type->setBody(fields);
    return type;
  }
  if (!type->isLayoutIdentical(StructType::get(context, fields)))
    report_fatal_error("[HotfixPass] incompatible struct.HFDescriptor layout",
                       false);
  return type;
}

FunctionCallee getRuntimeInvoke(Module &module) {
  LLVMContext &context = module.getContext();
  Type *i32 = Type::getInt32Ty(context);
  Type *pointer = PointerType::getUnqual(context);
  FunctionType *type = FunctionType::get(i32, {pointer}, false);
  return module.getOrInsertFunction("hf_vm_invoke", type);
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
  for (Argument &argument : function.args()) {
    argument.removeAttr(Attribute::Returned);
    if (argument.hasAttribute(Attribute::SwiftSelf)) {
      argument.removeAttr(Attribute::NoCapture);
      argument.removeAttr(Attribute::ReadNone);
      argument.removeAttr(Attribute::ReadOnly);
      argument.removeAttr(Attribute::WriteOnly);
    }
  }

  function.removeFnAttr(Attribute::AlwaysInline);
  function.addFnAttr(Attribute::NoInline);
}

uint32_t scalarKind(Type *type) {
  return type->isIntegerTy(64) ? HFValueKindSignedInteger : HFValueKindBool;
}

uint32_t returnKind(Type *type) {
  if (type->isIntegerTy(64))
    return HFValueKindSignedInteger;
  if (type->isIntegerTy(1))
    return HFValueKindBool;
  return HFValueKindVoid;
}

GlobalVariable *createDescriptor(Function &function, const FunctionShape &shape,
                                 uint64_t targetID, uint64_t signatureID) {
  Module &module = *function.getParent();
  LLVMContext &context = module.getContext();
  Type *i32 = Type::getInt32Ty(context);
  Type *i64 = Type::getInt64Ty(context);
  Type *pointer = PointerType::getUnqual(context);
  StructType *descriptorType = getHFDescriptorType(context);

  std::string uniqueSuffix =
      utohexstr(targetID, true, 16) + "." + utohexstr(signatureID, true, 16);
  Constant *nameData =
      ConstantDataArray::getString(context, function.getName(), true);
  auto *name = new GlobalVariable(module, nameData->getType(), true,
                                  GlobalValue::PrivateLinkage, nameData,
                                  "__ir_hotfix_name." + uniqueSuffix);

  Constant *kindsPointer = ConstantPointerNull::get(cast<PointerType>(pointer));
  if (!shape.scalarArguments.empty()) {
    SmallVector<uint32_t> kinds;
    kinds.reserve(shape.scalarArguments.size());
    for (const Argument *argument : shape.scalarArguments)
      kinds.push_back(scalarKind(argument->getType()));
    Constant *kindsData = ConstantDataArray::get(context, kinds);
    auto *kindsGlobal = new GlobalVariable(
        module, kindsData->getType(), true, GlobalValue::PrivateLinkage,
        kindsData, "__ir_hotfix_kinds." + uniqueSuffix);
    kindsPointer = kindsGlobal;
  }

  Constant *fields[] = {
      ConstantInt::get(i32, HF_ABI_VERSION),
      ConstantInt::get(i32, sizeof(HFDescriptor)),
      ConstantInt::get(i64, targetID),
      ConstantInt::get(i64, signatureID),
      ConstantInt::get(i32, returnKind(function.getReturnType())),
      ConstantInt::get(i32, shape.scalarArguments.size()),
      ConstantInt::get(i32, shape.receiver == nullptr
                                ? HFDescriptorFlagNone
                                : HFDescriptorFlagHasReceiver),
      ConstantInt::get(i32, 0),
      name,
      kindsPointer,
  };
  Constant *initializer = ConstantStruct::get(descriptorType, fields);
  auto *descriptor = new GlobalVariable(
      module, descriptorType, true, GlobalValue::PrivateLinkage, initializer,
      "__ir_hotfix_descriptor." + uniqueSuffix);
  descriptor->setSection(DescriptorSection);
  descriptor->setAlignment(Align(8));
  return descriptor;
}

struct InstrumentedFunction {
  Function *clone;
  GlobalVariable *descriptor;
};

InstrumentedFunction instrument(Function &function, const FunctionShape &shape,
                                FunctionCallee runtimeInvoke) {
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

  Type *i64 = builder.getInt64Ty();
  PointerType *pointer = builder.getPtrTy();
  StructType *valueType = getHFValueType(context);
  StructType *handleType = getHFHandleType(context);
  StructType *frameType = getHFPatchFrameType(context);
  const unsigned argumentCount = shape.scalarArguments.size();

  Value *argumentsPointer = ConstantPointerNull::get(pointer);
  if (argumentCount != 0) {
    ArrayType *argumentsType = ArrayType::get(valueType, argumentCount);
    AllocaInst *arguments = builder.CreateAlloca(
        argumentsType, nullptr, "hotfix.arguments");
    arguments->setAlignment(Align(8));
    Value *firstIndices[] = {builder.getInt32(0), builder.getInt32(0)};
    argumentsPointer = builder.CreateInBoundsGEP(
        argumentsType, arguments, firstIndices, "hotfix.arguments.pointer");

    unsigned index = 0;
    for (Argument *argument : shape.scalarArguments) {
      Value *indices[] = {builder.getInt32(0), builder.getInt32(index)};
      Value *slot = builder.CreateInBoundsGEP(
          argumentsType, arguments, indices, "hotfix.argument");
      builder.CreateStore(
          builder.getInt32(scalarKind(argument->getType())),
          builder.CreateStructGEP(valueType, slot, 0,
                                  "hotfix.argument.kind"));
      builder.CreateStore(
          builder.getInt32(HFValueFlagNone),
          builder.CreateStructGEP(valueType, slot, 1,
                                  "hotfix.argument.flags"));

      Value *encoded = argument;
      if (argument->getType()->isIntegerTy(1))
        encoded =
            builder.CreateZExt(argument, i64, "hotfix.argument.bits.value");
      StoreInst *bitsStore = builder.CreateStore(
          encoded, builder.CreateStructGEP(valueType, slot, 2,
                                            "hotfix.argument.bits"));
      bitsStore->setAlignment(Align(8));
      builder.CreateStore(
          ConstantPointerNull::get(pointer),
          builder.CreateStructGEP(valueType, slot, 3,
                                  "hotfix.argument.bytes"));
      StoreInst *byteCountStore = builder.CreateStore(
          builder.getInt64(0),
          builder.CreateStructGEP(valueType, slot, 4,
                                  "hotfix.argument.byte_count"));
      byteCountStore->setAlignment(Align(8));
      ++index;
    }
  }

  uint64_t targetID = fnv1a64(function.getName());
  uint64_t signatureID = fnv1a64(canonicalSignature(function, shape));
  AllocaInst *frame =
      builder.CreateAlloca(frameType, nullptr, "hotfix.frame");
  frame->setAlignment(Align(8));
  builder.CreateStore(
      builder.getInt32(HF_ABI_VERSION),
      builder.CreateStructGEP(frameType, frame, 0, "hotfix.frame.abi_version"));
  builder.CreateStore(
      builder.getInt32(sizeof(HFPatchFrame)),
      builder.CreateStructGEP(frameType, frame, 1, "hotfix.frame.struct_size"));
  StoreInst *targetStore = builder.CreateStore(
      builder.getInt64(targetID),
      builder.CreateStructGEP(frameType, frame, 2, "hotfix.frame.target_id"));
  targetStore->setAlignment(Align(8));
  StoreInst *signatureStore = builder.CreateStore(
      builder.getInt64(signatureID),
      builder.CreateStructGEP(frameType, frame, 3,
                              "hotfix.frame.signature_id"));
  signatureStore->setAlignment(Align(8));
  builder.CreateStore(
      argumentsPointer,
      builder.CreateStructGEP(frameType, frame, 4, "hotfix.frame.arguments"));
  builder.CreateStore(
      builder.getInt32(argumentCount),
      builder.CreateStructGEP(frameType, frame, 5,
                              "hotfix.frame.argument_count"));
  builder.CreateStore(
      builder.getInt32(shape.receiver == nullptr ? HFPatchFrameFlagNone
                                                 : HFPatchFrameFlagHasReceiver),
      builder.CreateStructGEP(frameType, frame, 6, "hotfix.frame.flags"));

  Value *receiverSlot =
      builder.CreateStructGEP(frameType, frame, 7, "hotfix.frame.receiver");
  Value *receiverToken = builder.getInt64(0);
  if (shape.receiver != nullptr)
    receiverToken = builder.CreatePtrToInt(shape.receiver, i64,
                                           "hotfix.receiver.token");
  StoreInst *tokenStore = builder.CreateStore(
      receiverToken,
      builder.CreateStructGEP(handleType, receiverSlot, 0,
                              "hotfix.receiver.handle_token"));
  tokenStore->setAlignment(Align(8));
  builder.CreateStore(
      builder.getInt32(0),
      builder.CreateStructGEP(handleType, receiverSlot, 1,
                              "hotfix.receiver.generation"));
  builder.CreateStore(
      builder.getInt16(shape.receiver == nullptr ? HFHandleKindInvalid
                                                 : HFHandleKindObject),
      builder.CreateStructGEP(handleType, receiverSlot, 2,
                              "hotfix.receiver.kind"));
  builder.CreateStore(
      builder.getInt16(shape.receiver == nullptr
                           ? HFHandleFlagNone
                           : HFHandleFlagBorrowed | HFHandleFlagBorrowedAddress),
      builder.CreateStructGEP(handleType, receiverSlot, 3,
                              "hotfix.receiver.flags"));

  Value *resultSlot =
      builder.CreateStructGEP(frameType, frame, 8, "hotfix.frame.result");
  builder.CreateStore(
      builder.getInt32(HFValueKindInvalid),
      builder.CreateStructGEP(valueType, resultSlot, 0,
                              "hotfix.result.kind"));
  builder.CreateStore(
      builder.getInt32(HFValueFlagNone),
      builder.CreateStructGEP(valueType, resultSlot, 1,
                              "hotfix.result.flags"));
  StoreInst *initialBitsStore = builder.CreateStore(
      builder.getInt64(0),
      builder.CreateStructGEP(valueType, resultSlot, 2,
                              "hotfix.result.bits"));
  initialBitsStore->setAlignment(Align(8));
  builder.CreateStore(
      ConstantPointerNull::get(pointer),
      builder.CreateStructGEP(valueType, resultSlot, 3,
                              "hotfix.result.bytes"));
  StoreInst *initialByteCountStore = builder.CreateStore(
      builder.getInt64(0),
      builder.CreateStructGEP(valueType, resultSlot, 4,
                              "hotfix.result.byte_count"));
  initialByteCountStore->setAlignment(Align(8));
  builder.CreateStore(
      builder.getInt32(HFStatusInvalidFrame),
      builder.CreateStructGEP(frameType, frame, 9, "hotfix.frame.status"));
  builder.CreateStore(
      builder.getInt32(0),
      builder.CreateStructGEP(frameType, frame, 10, "hotfix.frame.reserved"));

  CallInst *status =
      builder.CreateCall(runtimeInvoke, {frame}, "hotfix.status");
  Value *applied = builder.CreateICmpEQ(
      status, builder.getInt32(HFStatusApplied), "hotfix.applied");
  builder.CreateCondBr(applied, patched, fallback);

  builder.SetInsertPoint(patched);
  if (function.getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    LoadInst *loaded = builder.CreateLoad(
        i64,
        builder.CreateStructGEP(valueType, resultSlot, 2,
                                "hotfix.result.bits.patched"),
        "hotfix.result.value");
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
  GlobalVariable *descriptor =
      createDescriptor(function, shape, targetID, signatureID);
  return {clone, descriptor};
}

class HotfixPass : public PassInfoMixin<HotfixPass> {
public:
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    if (module.getModuleFlag(InstrumentedModuleFlag) != nullptr)
      return PreservedAnalyses::all();

    StringSet<> receiverSymbols = loadClassReceiverManifest();
    module.addModuleFlag(Module::Warning, InstrumentedModuleFlag, 1);
    addLoadMarker(module);

    struct EligibleFunction {
      Function *function;
      FunctionShape shape;
    };
    SmallVector<EligibleFunction> eligibleFunctions;
    for (Function &function : module) {
      FunctionShape shape = classifyFunction(function, receiverSymbols);
      std::string cloneName = (function.getName() + OriginalSuffix).str();
      if (shape.classification == FunctionClassification::Eligible &&
          module.getFunction(cloneName) == nullptr) {
        eligibleFunctions.push_back({&function, std::move(shape)});
      } else if (shape.classification == FunctionClassification::Skipped) {
        errs() << "[HotfixPass] skip " << function.getName() << ": "
               << shape.skipReason << '\n';
      }
    }

    if (!eligibleFunctions.empty()) {
      FunctionCallee runtimeInvoke = getRuntimeInvoke(module);
      SmallVector<GlobalValue *> retainedFunctions;
      SmallVector<GlobalValue *> retainedDescriptors;
      retainedFunctions.reserve(eligibleFunctions.size() * 2);
      retainedDescriptors.reserve(eligibleFunctions.size());
      for (EligibleFunction &eligible : eligibleFunctions) {
        InstrumentedFunction instrumented =
            instrument(*eligible.function, eligible.shape, runtimeInvoke);
        retainedFunctions.push_back(eligible.function);
        retainedFunctions.push_back(instrumented.clone);
        retainedDescriptors.push_back(instrumented.descriptor);
      }
      appendToCompilerUsed(module, retainedFunctions);
      appendToUsed(module, retainedDescriptors);
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
