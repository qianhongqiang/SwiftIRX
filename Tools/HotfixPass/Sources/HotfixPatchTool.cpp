#include "IRHotfixABI.h"
#include "HFIRLowering.h"
#include "HFPatchContainer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace {
constexpr uint32_t ManifestSchemaVersion = 1;
constexpr StringLiteral PatchEntryName = "__ir_hotfix_patch_entry";
constexpr StringLiteral SourcePatchEntryName = "hotfixPatch";

struct Target {
  std::string symbol;
  uint64_t targetID = 0;
  uint64_t signatureID = 0;
  std::string returnKind;
  std::vector<std::string> argumentKinds;
  bool hasReceiver = false;
  irhotfix::hfir::TargetReceiverKind receiverKind =
      irhotfix::hfir::TargetReceiverKind::None;
};

std::optional<irhotfix::hfir::TargetReceiverKind>
parseReceiverKind(StringRef kind) {
  using Kind = irhotfix::hfir::TargetReceiverKind;
  if (kind == "none") return Kind::None;
  if (kind == "object") return Kind::Object;
  if (kind == "native") return Kind::Native;
  return std::nullopt;
}

uint64_t fnv1a64(StringRef text) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : text.bytes()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool parseHexID(StringRef text, uint64_t &value) {
  return text.size() == 18 && text.starts_with("0x") &&
         to_integer(text.drop_front(2), value, 16);
}

bool isValueKind(StringRef kind, bool allowVoid) {
  return kind == "i64" || kind == "i1" || kind == "f32" || kind == "f64" ||
         kind == "object" || kind == "string" ||
         (allowVoid && kind == "void");
}

std::optional<irhotfix::hfir::TargetValueKind>
targetValueKind(StringRef kind) {
  using Kind = irhotfix::hfir::TargetValueKind;
  if (kind == "void") return Kind::Void;
  if (kind == "i1") return Kind::Bool;
  if (kind == "i64") return Kind::I64;
  if (kind == "f32") return Kind::F32;
  if (kind == "f64") return Kind::F64;
  if (kind == "object") return Kind::Object;
  if (kind == "string") return Kind::String;
  return std::nullopt;
}

bool makeTargetABI(const Target &target,
                   irhotfix::hfir::TargetABISchema &schema,
                   std::string &error) {
  const auto returnType = targetValueKind(target.returnKind);
  if (!returnType) {
    error = "target '" + target.symbol + "' has an invalid ABI return kind";
    return false;
  }
  schema = {};
  schema.returnType = *returnType;
  schema.receiverKind = target.receiverKind;
  for (const std::string &name : target.argumentKinds) {
    const auto type = targetValueKind(name);
    if (!type) {
      error = "target '" + target.symbol +
              "' has an invalid ABI argument kind";
      return false;
    }
    schema.parameterTypes.push_back(*type);
  }
  return true;
}

std::string canonicalSignature(const Target &target) {
  std::string signature = "return=" + target.returnKind + ";arguments=";
  for (size_t index = 0; index < target.argumentKinds.size(); ++index) {
    if (index != 0)
      signature += ',';
    signature += target.argumentKinds[index];
  }
  signature += target.hasReceiver ? ";receiver=1" : ";receiver=0";
  return signature;
}

bool parseTarget(const json::Object &object, Target &target,
                 std::string &error) {
  std::optional<StringRef> symbol = object.getString("symbol");
  std::optional<StringRef> targetID = object.getString("targetID");
  std::optional<StringRef> signatureID = object.getString("signatureID");
  std::optional<StringRef> returnKind = object.getString("returnKind");
  std::optional<bool> hasReceiver = object.getBoolean("hasReceiver");
  const json::Array *argumentKinds = object.getArray("argumentKinds");
  if (!symbol || !targetID || !signatureID || !returnKind || !hasReceiver ||
      argumentKinds == nullptr) {
    error = "target manifest contains an incomplete target";
    return false;
  }

  target.symbol = symbol->str();
  target.returnKind = returnKind->str();
  target.hasReceiver = *hasReceiver;
  if (std::optional<StringRef> receiverName = object.getString("receiverKind")) {
    const auto parsed = parseReceiverKind(*receiverName);
    if (!parsed) {
      error = "target '" + target.symbol + "' has an invalid receiver kind";
      return false;
    }
    target.receiverKind = *parsed;
  } else {
    target.receiverKind = target.hasReceiver
                              ? irhotfix::hfir::TargetReceiverKind::Object
                              : irhotfix::hfir::TargetReceiverKind::None;
  }
  if (target.hasReceiver !=
      (target.receiverKind != irhotfix::hfir::TargetReceiverKind::None)) {
    error = "target '" + target.symbol +
            "' has inconsistent hasReceiver and receiverKind";
    return false;
  }
  if (target.symbol.empty() || !parseHexID(*targetID, target.targetID) ||
      !parseHexID(*signatureID, target.signatureID)) {
    error = "target manifest contains an invalid symbol or ID";
    return false;
  }
  if (!isValueKind(target.returnKind, true)) {
    error = "target '" + target.symbol + "' has an unsupported return kind";
    return false;
  }
  for (const json::Value &value : *argumentKinds) {
    std::optional<StringRef> kind = value.getAsString();
    if (!kind || !isValueKind(*kind, false)) {
      error = "target '" + target.symbol + "' has an unsupported argument kind";
      return false;
    }
    target.argumentKinds.push_back(kind->str());
  }
  if (target.argumentKinds.size() > HF_MAX_SCALAR_ARGUMENT_COUNT) {
    error = "target '" + target.symbol + "' has too many arguments";
    return false;
  }
  if (fnv1a64(target.symbol) != target.targetID ||
      fnv1a64(canonicalSignature(target)) != target.signatureID) {
    error = "target '" + target.symbol + "' has invalid descriptor hashes";
    return false;
  }
  return true;
}

bool loadTarget(StringRef manifestPath, StringRef query, Target &selected,
                std::string &error) {
  auto buffer = MemoryBuffer::getFile(manifestPath);
  if (!buffer) {
    error = "cannot read target manifest '" + manifestPath.str() +
            "': " + buffer.getError().message();
    return false;
  }
  Expected<json::Value> parsed = json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    error = "cannot parse target manifest '" + manifestPath.str() +
            "': " + toString(parsed.takeError());
    return false;
  }
  const json::Object *root = parsed->getAsObject();
  if (root == nullptr ||
      root->getInteger("schemaVersion") != ManifestSchemaVersion ||
      root->getInteger("abiVersion") != HF_ABI_VERSION) {
    error = "target manifest has an unsupported schema or ABI version";
    return false;
  }
  const json::Array *targets = root->getArray("targets");
  if (targets == nullptr) {
    error = "target manifest has no targets array";
    return false;
  }

  std::vector<Target> exactMatches;
  std::vector<Target> partialMatches;
  for (const json::Value &value : *targets) {
    const json::Object *object = value.getAsObject();
    if (object == nullptr) {
      error = "target manifest contains a non-object target";
      return false;
    }
    Target candidate;
    if (!parseTarget(*object, candidate, error))
      return false;
    const bool exact =
        candidate.symbol == query || (object->getString("targetID") &&
                                      *object->getString("targetID") == query);
    if (exact)
      exactMatches.push_back(std::move(candidate));
    else if (candidate.symbol.find(query.str()) != std::string::npos)
      partialMatches.push_back(std::move(candidate));
  }

  std::vector<Target> &matches =
      exactMatches.empty() ? partialMatches : exactMatches;
  if (matches.empty()) {
    error = "no manifest target matches '" + query.str() + "'";
    return false;
  }
  if (matches.size() != 1) {
    error = "manifest target query '" + query.str() + "' is ambiguous:";
    for (const Target &target : matches)
      error += "\n  " + target.symbol;
    return false;
  }
  selected = std::move(matches.front());
  return true;
}

bool matchesKind(Type *type, StringRef kind) {
  if (kind == "void")
    return type->isVoidTy();
  if (kind == "i64")
    return type->isIntegerTy(64);
  if (kind == "i1")
    return type->isIntegerTy(1);
  if (kind == "f32")
    return type->isFloatTy();
  if (kind == "f64")
    return type->isDoubleTy();
  if (kind == "object")
    return type->isPointerTy();
  // Native Swift.String is not a stable pointer ABI. It requires the explicit
  // String boundary adapter and is deliberately rejected until that adapter
  // rewrites the patch entry to the canonical string-handle form.
  if (kind == "string")
    return false;
  return false;
}

bool validateEntry(Function &entry, const Target &target, std::string &error) {
  if (entry.isDeclaration()) {
    error = "patch module declares but does not define hotfixPatch";
    return false;
  }
  if (entry.isVarArg()) {
    error = "hotfixPatch cannot be variadic";
    return false;
  }
  if (target.returnKind == "string" ||
      llvm::is_contained(target.argumentKinds, "string")) {
    error = "target '" + target.symbol +
            "' uses Swift.String; the canonical string-handle schema is "
            "available, but Swift.String entry rewriting is not implemented";
    return false;
  }
  if (!matchesKind(entry.getReturnType(), target.returnKind)) {
    error =
        "hotfixPatch return type does not match target '" + target.symbol + "'";
    return false;
  }

  const size_t expectedCount =
      target.argumentKinds.size() +
      (target.receiverKind == irhotfix::hfir::TargetReceiverKind::None ? 0 : 1);
  if (entry.arg_size() != expectedCount) {
    error = "hotfixPatch parameter count does not match target '" +
            target.symbol + "'";
    return false;
  }

  auto argument = entry.arg_begin();
  if (target.receiverKind != irhotfix::hfir::TargetReceiverKind::None) {
    if (!argument->getType()->isPointerTy()) {
      error =
          "hotfixPatch must receive the target receiver as its first parameter";
      return false;
    }
    ++argument;
  }
  for (StringRef kind : target.argumentKinds) {
    if (!matchesKind(argument->getType(), kind)) {
      error = "hotfixPatch scalar parameters do not match target '" +
              target.symbol + "'";
      return false;
    }
    ++argument;
  }

  return true;
}

bool lowerCheckedIntegerArithmeticInFunction(Function &entry,
                                             std::string &error) {
  SmallVector<Instruction *, 16> erase;
  for (BasicBlock &block : entry) {
    for (Instruction &instruction : block) {
      auto *intrinsic = dyn_cast<IntrinsicInst>(&instruction);
      if (intrinsic == nullptr)
        continue;

      if (intrinsic->getIntrinsicID() == Intrinsic::expect) {
        intrinsic->replaceAllUsesWith(intrinsic->getArgOperand(0));
        erase.push_back(intrinsic);
        continue;
      }

      Instruction::BinaryOps opcode;
      switch (intrinsic->getIntrinsicID()) {
      case Intrinsic::sadd_with_overflow:
      case Intrinsic::uadd_with_overflow:
        opcode = Instruction::Add;
        break;
      case Intrinsic::ssub_with_overflow:
      case Intrinsic::usub_with_overflow:
        opcode = Instruction::Sub;
        break;
      case Intrinsic::smul_with_overflow:
      case Intrinsic::umul_with_overflow:
        opcode = Instruction::Mul;
        break;
      default:
        continue;
      }

      IRBuilder<> builder(intrinsic);
      Value *result = builder.CreateBinOp(opcode, intrinsic->getArgOperand(0),
                                          intrinsic->getArgOperand(1));
      for (User *user : make_early_inc_range(intrinsic->users())) {
        auto *extract = dyn_cast<ExtractValueInst>(user);
        if (extract == nullptr || extract->getNumIndices() != 1) {
          error = "checked integer operation has an unsupported aggregate use";
          return false;
        }
        switch (*extract->idx_begin()) {
        case 0:
          extract->replaceAllUsesWith(result);
          break;
        case 1:
          extract->replaceAllUsesWith(
              ConstantInt::getFalse(entry.getContext()));
          break;
        default:
          error = "checked integer operation has an invalid result index";
          return false;
        }
        erase.push_back(extract);
      }
      erase.push_back(intrinsic);
    }
  }
  for (Instruction *instruction : erase)
    instruction->eraseFromParent();
  for (BasicBlock &block : entry)
    ConstantFoldTerminator(&block, true);
  removeUnreachableBlocks(entry);
  return true;
}

bool lowerCheckedIntegerArithmetic(Function &entry, std::string &error) {
  std::set<Function *> visited;
  std::vector<Function *> worklist = {&entry};
  while (!worklist.empty()) {
    Function *function = worklist.back();
    worklist.pop_back();
    if (!visited.insert(function).second)
      continue;
    for (BasicBlock &block : *function) {
      for (Instruction &instruction : block) {
        auto *call = dyn_cast<CallBase>(&instruction);
        Function *callee = call == nullptr ? nullptr : call->getCalledFunction();
        if (callee != nullptr && !callee->isDeclaration() &&
            !callee->isIntrinsic() &&
            !callee->getName().contains("__ir_hotfix_patch_anchor_") &&
            (callee->hasLocalLinkage() ||
             callee->getName().starts_with("$s13IRHotfixPatch")))
          worklist.push_back(callee);
      }
    }
    if (!lowerCheckedIntegerArithmeticInFunction(*function, error))
      return false;
  }
  return true;
}

void nameAnonymousBlocks(Function &entry) {
  unsigned blockIndex = 0;
  for (BasicBlock &block : entry) {
    if (!block.hasName()) {
      block.setName(blockIndex == 0
                        ? "entry"
                        : "hotfix.bb." + std::to_string(blockIndex));
    }
    ++blockIndex;
  }
}

bool writePatch(Function &entry, StringRef outputPath, std::string &error) {
  entry.setLinkage(GlobalValue::ExternalLinkage);
  entry.setVisibility(GlobalValue::DefaultVisibility);
  nameAnonymousBlocks(entry);

  std::error_code fileError;
  raw_fd_ostream output(outputPath, fileError);
  if (fileError) {
    error = "cannot open patch output '" + outputPath.str() +
            "': " + fileError.message();
    return false;
  }
  entry.print(output);
  std::set<const Function *> written = {&entry};
  std::vector<const Function *> worklist = {&entry};
  while (!worklist.empty()) {
    const Function *function = worklist.back();
    worklist.pop_back();
    for (const BasicBlock &block : *function) {
      for (const Instruction &instruction : block) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        const Function *callee = call == nullptr ? nullptr
                                                  : call->getCalledFunction();
        if (callee == nullptr || callee->isDeclaration() ||
            callee->isIntrinsic() ||
            callee->getName().contains("__ir_hotfix_patch_anchor_") ||
            (!callee->hasLocalLinkage() &&
             !callee->getName().starts_with("$s13IRHotfixPatch")) ||
            !written.insert(callee).second)
          continue;
        output << "\n\n";
        callee->print(output);
        worklist.push_back(callee);
      }
    }
  }
  output.flush();
  if (output.has_error()) {
    error = "cannot write patch output '" + outputPath.str() + "'";
    return false;
  }
  return true;
}

bool writeHFPatch(Module &module, Function &entry, const Target &target,
                  StringRef outputPath, std::string &error) {
  irhotfix::hfir::Package package;
  irhotfix::hfir::TargetABISchema targetABI;
  if (!makeTargetABI(target, targetABI, error))
    return false;
  if (!irhotfix::lowering::lowerFunction(
          module, entry, target.targetID, target.signatureID, targetABI,
          package, error))
    return false;
  std::vector<std::uint8_t> bytes;
  if (!irhotfix::container::encode(package, bytes, error))
    return false;
  return irhotfix::container::writeFile(outputPath.str(), bytes, error);
}

Function *findSourcePatchEntry(Module &module, std::string &error) {
  if (Function *entry = module.getFunction(PatchEntryName))
    return entry;
  if (Function *entry = module.getFunction(SourcePatchEntryName))
    return entry;

  Function *candidate = nullptr;
  for (Function &function : module) {
    if (function.isDeclaration() ||
        !function.getName().contains(SourcePatchEntryName))
      continue;
    if (candidate != nullptr) {
      error = "patch source defines more than one hotfixPatch candidate";
      return nullptr;
    }
    candidate = &function;
  }
  if (candidate == nullptr)
    error = "patch source must define exactly one top-level function named "
            "hotfixPatch";
  return candidate;
}

bool buildPatch(StringRef manifestPath, StringRef query, StringRef inputPath,
                StringRef outputPath, std::string &error) {
  Target target;
  if (!loadTarget(manifestPath, query, target, error))
    return false;

  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module = parseIRFile(inputPath, diagnostic, context);
  if (module == nullptr) {
    std::string message;
    raw_string_ostream stream(message);
    diagnostic.print("HotfixPatchTool", stream);
    stream.flush();
    error = "cannot read patch LLVM module '" + inputPath.str() + "': " +
            message;
    return false;
  }

  Function *entry = findSourcePatchEntry(*module, error);
  if (entry == nullptr)
    return false;
  if (!validateEntry(*entry, target, error))
    return false;
  if (!lowerCheckedIntegerArithmetic(*entry, error))
    return false;
  if (module->getFunction(target.symbol) != nullptr) {
    error = "patch module already defines target symbol '" + target.symbol +
            "'";
    return false;
  }

  entry->setName(target.symbol);
  if (outputPath.contains(".hfpatch"))
    return writeHFPatch(*module, *entry, target, outputPath, error);
  return writePatch(*entry, outputPath, error);
}

bool lowerHFPatch(StringRef manifestPath, StringRef query, StringRef inputPath,
                  StringRef outputPath, std::string &error) {
  Target target;
  if (!loadTarget(manifestPath, query, target, error))
    return false;
  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module = parseIRFile(inputPath, diagnostic, context);
  if (module == nullptr) {
    std::string message;
    raw_string_ostream stream(message);
    diagnostic.print("HotfixPatchTool", stream);
    stream.flush();
    error = "cannot read LLVM patch '" + inputPath.str() + "': " + message;
    return false;
  }
  Function *entry = module->getFunction(target.symbol);
  if (entry == nullptr) {
    error = "LLVM patch does not define baseline target '" + target.symbol + "'";
    return false;
  }
  if (!validateEntry(*entry, target, error) ||
      !lowerCheckedIntegerArithmetic(*entry, error))
    return false;
  return writeHFPatch(*module, *entry, target, outputPath, error);
}

bool extractAnnotatedPatches(StringRef manifestPath, StringRef inputPath,
                             StringRef outputDirectory, std::string &error) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module = parseIRFile(inputPath, diagnostic, context);
  if (module == nullptr) {
    std::string message;
    raw_string_ostream stream(message);
    diagnostic.print("HotfixPatchTool", stream);
    stream.flush();
    error =
        "cannot read Swift LLVM module '" + inputPath.str() + "': " + message;
    return false;
  }

  std::vector<Function *> anchors;
  for (Function &function : *module) {
    if (!function.isDeclaration() &&
        function.getName().contains("__ir_hotfix_patch_anchor_")) {
      anchors.push_back(&function);
    }
  }

  std::error_code directoryError = sys::fs::create_directories(outputDirectory);
  if (directoryError) {
    error = "cannot create patch output directory '" + outputDirectory.str() +
            "': " + directoryError.message();
    return false;
  }

  std::set<uint64_t> extractedTargetIDs;
  for (Function *anchor : anchors) {
    std::set<Function *> callees;
    for (BasicBlock &block : *anchor) {
      for (Instruction &instruction : block) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        Function *callee =
            call == nullptr ? nullptr : call->getCalledFunction();
        if (callee != nullptr && callee != anchor && !callee->isDeclaration() &&
            !callee->getName().contains("__ir_hotfix_patch_anchor_")) {
          callees.insert(callee);
        }
      }
    }
    if (callees.size() != 1) {
      error = "annotation anchor '" + anchor->getName().str() +
              "' must call exactly one defined target function";
      return false;
    }

    Function *targetFunction = *callees.begin();
    Target target;
    if (!loadTarget(manifestPath, targetFunction->getName(), target, error))
      return false;
    if (target.symbol != targetFunction->getName()) {
      error = "annotated function '" + targetFunction->getName().str() +
              "' is not an exact baseline Manifest target";
      return false;
    }
    if (!extractedTargetIDs.insert(target.targetID).second) {
      error = "multiple @HotfixPatch annotations resolve to target '" +
              target.symbol + "'";
      return false;
    }
    if (!validateEntry(*targetFunction, target, error) ||
        !lowerCheckedIntegerArithmetic(*targetFunction, error)) {
      return false;
    }

    SmallString<256> outputPath(outputDirectory);
    sys::path::append(outputPath,
                      "0x" + utohexstr(target.targetID, true, 16) + ".irpatch");
    if (!writePatch(*targetFunction, outputPath, error))
      return false;

    SmallString<256> packagePath(outputDirectory);
    sys::path::append(packagePath,
                      "0x" + utohexstr(target.targetID, true, 16) + ".hfpatch");
    if (!writeHFPatch(*module, *targetFunction, target, packagePath, error))
      return false;
  }
  return true;
}

int fail(StringRef message) {
  errs() << "[HotfixPatch] error: " << message << '\n';
  return 1;
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 6 && StringRef(argv[1]) == "build") {
    std::string error;
    if (!buildPatch(argv[2], argv[3], argv[4], argv[5], error))
      return fail(error);
    return 0;
  }
  if (argc == 5 && StringRef(argv[1]) == "extract-annotated") {
    std::string error;
    if (!extractAnnotatedPatches(argv[2], argv[3], argv[4], error))
      return fail(error);
    return 0;
  }
  if (argc == 6 && StringRef(argv[1]) == "lower-hfir") {
    std::string error;
    if (!lowerHFPatch(argv[2], argv[3], argv[4], argv[5], error))
      return fail(error);
    return 0;
  }
  {
    return fail("usage: HotfixPatchTool build <manifest.json> <target-query> "
                "<swift.bc|swift.ll> <output.irpatch|output.hfpatch>\n"
                "       HotfixPatchTool extract-annotated <manifest.json> "
                "<swift.bc|swift.ll> <output-directory>\n"
                "       HotfixPatchTool lower-hfir <manifest.json> "
                "<target-query> <input.irpatch|swift.ll> <output.hfpatch>");
  }
}
