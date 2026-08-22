#include "IRHotfixABI.h"

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
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace {
constexpr uint32_t ManifestSchemaVersion = 1;
constexpr StringLiteral PatchEntryName = "__ir_hotfix_patch_entry";

struct Target {
  std::string symbol;
  uint64_t targetID = 0;
  uint64_t signatureID = 0;
  std::string returnKind;
  std::vector<std::string> argumentKinds;
  bool hasReceiver = false;
};

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
  return kind == "i64" || kind == "i1" || (allowVoid && kind == "void");
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

  std::vector<Target> matches;
  for (const json::Value &value : *targets) {
    const json::Object *object = value.getAsObject();
    if (object == nullptr) {
      error = "target manifest contains a non-object target";
      return false;
    }
    Target candidate;
    if (!parseTarget(*object, candidate, error))
      return false;
    if (candidate.symbol == query ||
        candidate.symbol.find(query.str()) != std::string::npos ||
        (object->getString("targetID") &&
         *object->getString("targetID") == query)) {
      matches.push_back(std::move(candidate));
    }
  }

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
  return false;
}

bool validateEntry(Function &entry, const Target &target, std::string &error) {
  if (entry.isDeclaration()) {
    error = "Swift module declares but does not define hotfixPatch";
    return false;
  }
  if (entry.isVarArg()) {
    error = "hotfixPatch cannot be variadic";
    return false;
  }
  if (!matchesKind(entry.getReturnType(), target.returnKind)) {
    error =
        "hotfixPatch return type does not match target '" + target.symbol + "'";
    return false;
  }

  const size_t expectedCount =
      target.argumentKinds.size() + (target.hasReceiver ? 1 : 0);
  if (entry.arg_size() != expectedCount) {
    error = "hotfixPatch parameter count does not match target '" +
            target.symbol + "'";
    return false;
  }

  auto argument = entry.arg_begin();
  if (target.hasReceiver) {
    if (!argument->getType()->isPointerTy()) {
      error =
          "hotfixPatch must receive the target object as its first parameter";
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

  for (BasicBlock &block : entry) {
    for (Instruction &instruction : block) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (call == nullptr)
        continue;
      const Function *callee = call->getCalledFunction();
      if (callee != nullptr && callee != &entry && !callee->isDeclaration() &&
          (callee->hasLocalLinkage() ||
           callee->getName().starts_with("$s13IRHotfixPatch"))) {
        error = "hotfixPatch calls local Swift helper '" +
                callee->getName().str() +
                "'; inline the helper before building the patch";
        return false;
      }
    }
  }
  return true;
}

bool lowerCheckedIntegerArithmetic(Function &entry, std::string &error) {
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
    error =
        "cannot read Swift LLVM module '" + inputPath.str() + "': " + message;
    return false;
  }

  Function *entry = module->getFunction(PatchEntryName);
  if (entry == nullptr) {
    error = "Swift source must define exactly one top-level function named "
            "hotfixPatch";
    return false;
  }
  if (!validateEntry(*entry, target, error))
    return false;
  if (!lowerCheckedIntegerArithmetic(*entry, error))
    return false;
  if (module->getFunction(target.symbol) != nullptr) {
    error =
        "Swift module already defines target symbol '" + target.symbol + "'";
    return false;
  }

  entry->setName(target.symbol);
  entry->setLinkage(GlobalValue::ExternalLinkage);
  entry->setVisibility(GlobalValue::DefaultVisibility);
  unsigned blockIndex = 0;
  for (BasicBlock &block : *entry) {
    if (!block.hasName()) {
      block.setName(blockIndex == 0
                        ? "entry"
                        : "hotfix.bb." + std::to_string(blockIndex));
    }
    ++blockIndex;
  }

  std::error_code fileError;
  raw_fd_ostream output(outputPath, fileError);
  if (fileError) {
    error = "cannot open patch output '" + outputPath.str() +
            "': " + fileError.message();
    return false;
  }
  entry->print(output);
  output.flush();
  if (output.has_error()) {
    error = "cannot write patch output '" + outputPath.str() + "'";
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
  if (argc != 6 || StringRef(argv[1]) != "build") {
    return fail("usage: HotfixPatchTool build <manifest.json> <target-query> "
                "<swift.bc|swift.ll> <output.irpatch>");
  }

  std::string error;
  if (!buildPatch(argv[2], argv[3], argv[4], argv[5], error))
    return fail(error);
  return 0;
}
