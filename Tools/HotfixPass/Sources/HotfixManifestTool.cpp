#include "IRHotfixABI.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {
constexpr uint32_t ManifestSchemaVersion = 1;
constexpr StringLiteral DescriptorSection = "__DATA,__hotfix";

enum class ReceiverKind { None, Object, Native };

struct Target {
  std::string symbol;
  uint64_t targetID = 0;
  uint64_t signatureID = 0;
  uint32_t returnKind = HFValueKindInvalid;
  std::vector<uint32_t> argumentKinds;
  bool hasReceiver = false;
  ReceiverKind receiverKind = ReceiverKind::None;

  bool operator==(const Target &other) const {
    return symbol == other.symbol && targetID == other.targetID &&
           signatureID == other.signatureID &&
           returnKind == other.returnKind &&
           argumentKinds == other.argumentKinds &&
           hasReceiver == other.hasReceiver &&
           receiverKind == other.receiverKind;
  }
};

StringRef receiverKindName(ReceiverKind kind) {
  switch (kind) {
  case ReceiverKind::None: return "none";
  case ReceiverKind::Object: return "object";
  case ReceiverKind::Native: return "native";
  }
  llvm_unreachable("invalid receiver kind");
}

std::optional<ReceiverKind> parseReceiverKind(StringRef name) {
  if (name == "none") return ReceiverKind::None;
  if (name == "object") return ReceiverKind::Object;
  if (name == "native") return ReceiverKind::Native;
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

std::optional<StringRef> abiName(uint32_t kind, bool allowVoid) {
  switch (kind) {
  case HFValueKindSignedInteger:
    return "i64";
  case HFValueKindBool:
    return "i1";
  case HFValueKindFloat32:
    return "f32";
  case HFValueKindFloat64:
    return "f64";
  case HFValueKindHostHandle:
    return "object";
  case HFValueKindStringHandle:
    return "string";
  case HFValueKindVoid:
    if (allowVoid)
      return "void";
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::string canonicalSignature(const Target &target) {
  std::string signature = "return=";
  signature += *abiName(target.returnKind, true);
  signature += ";arguments=";
  for (size_t index = 0; index < target.argumentKinds.size(); ++index) {
    if (index != 0)
      signature += ',';
    signature += *abiName(target.argumentKinds[index], false);
  }
  signature += target.hasReceiver ? ";receiver=1" : ";receiver=0";
  return signature;
}

std::string hexID(uint64_t value) {
  return "0x" + utohexstr(value, true, 16);
}

bool validateTarget(const Target &target, std::string &error) {
  if (target.symbol.empty()) {
    error = "descriptor has an empty symbol";
    return false;
  }
  if (!abiName(target.returnKind, true)) {
    error = "target '" + target.symbol + "' has an unsupported return kind";
    return false;
  }
  for (uint32_t kind : target.argumentKinds) {
    if (!abiName(kind, false)) {
      error = "target '" + target.symbol +
              "' has an unsupported argument kind";
      return false;
    }
  }
  if (target.argumentKinds.size() > HF_MAX_SCALAR_ARGUMENT_COUNT) {
    error = "target '" + target.symbol + "' has too many arguments";
    return false;
  }
  if (fnv1a64(target.symbol) != target.targetID) {
    error = "target '" + target.symbol + "' has a mismatched target ID";
    return false;
  }
  if (fnv1a64(canonicalSignature(target)) != target.signatureID) {
    error = "target '" + target.symbol + "' has a mismatched signature ID";
    return false;
  }
  return true;
}

bool addTarget(std::map<uint64_t, Target> &targets, Target target,
               std::string &error) {
  if (!validateTarget(target, error))
    return false;
  auto [iterator, inserted] = targets.emplace(target.targetID, target);
  if (!inserted && !(iterator->second == target)) {
    error = "target ID collision between '" + iterator->second.symbol +
            "' and '" + target.symbol + "'";
    return false;
  }
  return true;
}

bool constantInteger(const ConstantStruct &descriptor, unsigned index,
                     uint64_t &value, std::string &error) {
  const auto *integer = dyn_cast<ConstantInt>(descriptor.getOperand(index));
  if (integer == nullptr) {
    error = "descriptor field " + std::to_string(index) +
            " is not an integer constant";
    return false;
  }
  value = integer->getZExtValue();
  return true;
}

bool extractTarget(const GlobalVariable &global, Target &target,
                   std::string &error) {
  const auto *descriptor =
      dyn_cast_or_null<ConstantStruct>(global.getInitializer());
  if (descriptor == nullptr || descriptor->getNumOperands() != 10) {
    error = "global '" + global.getName().str() +
            "' has an invalid HFDescriptor initializer";
    return false;
  }

  uint64_t abiVersion = 0;
  uint64_t structSize = 0;
  uint64_t targetID = 0;
  uint64_t signatureID = 0;
  uint64_t returnKind = 0;
  uint64_t argumentCount = 0;
  uint64_t flags = 0;
  uint64_t reserved = 0;
  if (!constantInteger(*descriptor, 0, abiVersion, error) ||
      !constantInteger(*descriptor, 1, structSize, error) ||
      !constantInteger(*descriptor, 2, targetID, error) ||
      !constantInteger(*descriptor, 3, signatureID, error) ||
      !constantInteger(*descriptor, 4, returnKind, error) ||
      !constantInteger(*descriptor, 5, argumentCount, error) ||
      !constantInteger(*descriptor, 6, flags, error) ||
      !constantInteger(*descriptor, 7, reserved, error))
    return false;

  if (abiVersion != HF_ABI_VERSION) {
    error = "global '" + global.getName().str() +
            "' uses an unsupported ABI version";
    return false;
  }
  if (structSize != sizeof(HFDescriptor) || reserved != 0) {
    error = "global '" + global.getName().str() +
            "' has an invalid HFDescriptor layout";
    return false;
  }
  constexpr uint64_t KnownFlags = HFDescriptorFlagHasReceiver |
                                  HFDescriptorFlagNativeReceiver;
  if ((flags & ~KnownFlags) != 0) {
    error = "global '" + global.getName().str() +
            "' has unknown descriptor flags";
    return false;
  }
  if (argumentCount > HF_MAX_SCALAR_ARGUMENT_COUNT) {
    error = "global '" + global.getName().str() +
            "' has too many descriptor arguments";
    return false;
  }

  const auto *nameGlobal = dyn_cast<GlobalVariable>(
      descriptor->getOperand(8)->stripPointerCasts());
  const auto *nameData =
      nameGlobal == nullptr
          ? nullptr
          : dyn_cast_or_null<ConstantDataSequential>(nameGlobal->getInitializer());
  if (nameData == nullptr || !nameData->isCString()) {
    error = "global '" + global.getName().str() +
            "' has an invalid symbol string";
    return false;
  }

  std::vector<uint32_t> kinds;
  if (argumentCount == 0) {
    if (!isa<ConstantPointerNull>(descriptor->getOperand(9))) {
      error = "global '" + global.getName().str() +
              "' has an unexpected argument-kind array";
      return false;
    }
  } else {
    const auto *kindsGlobal = dyn_cast<GlobalVariable>(
        descriptor->getOperand(9)->stripPointerCasts());
    const auto *kindsData =
        kindsGlobal == nullptr
            ? nullptr
            : dyn_cast_or_null<ConstantDataSequential>(
                  kindsGlobal->getInitializer());
    if (kindsData == nullptr ||
        kindsData->getNumElements() != argumentCount) {
      error = "global '" + global.getName().str() +
              "' has an invalid argument-kind array";
      return false;
    }
    kinds.reserve(argumentCount);
    for (unsigned index = 0; index < argumentCount; ++index)
      kinds.push_back(kindsData->getElementAsInteger(index));
  }

  target.symbol = nameData->getAsCString().str();
  target.targetID = targetID;
  target.signatureID = signatureID;
  target.returnKind = returnKind;
  target.argumentKinds = std::move(kinds);
  target.hasReceiver = (flags & HFDescriptorFlagHasReceiver) != 0;
  if ((flags & HFDescriptorFlagNativeReceiver) != 0 && !target.hasReceiver) {
    error = "global '" + global.getName().str() +
            "' marks a native receiver without a receiver";
    return false;
  }
  target.receiverKind =
      !target.hasReceiver
          ? ReceiverKind::None
          : ((flags & HFDescriptorFlagNativeReceiver) != 0
                 ? ReceiverKind::Native
                 : ReceiverKind::Object);
  return true;
}

bool extractManifest(StringRef inputPath, std::map<uint64_t, Target> &targets,
                     std::string &error) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module = parseIRFile(inputPath, diagnostic, context);
  if (module == nullptr) {
    std::string message;
    raw_string_ostream stream(message);
    diagnostic.print("HotfixManifestTool", stream);
    stream.flush();
    error = "cannot read LLVM module '" + inputPath.str() + "': " + message;
    return false;
  }

  for (const GlobalVariable &global : module->globals()) {
    if (!global.hasSection() || global.getSection() != DescriptorSection)
      continue;
    const auto *type = dyn_cast<StructType>(global.getValueType());
    if (type == nullptr || !type->hasName() ||
        type->getName() != "struct.HFDescriptor") {
      error = "section '" + DescriptorSection.str() +
              "' contains a non-HFDescriptor global";
      return false;
    }
    Target target;
    if (!extractTarget(global, target, error) ||
        !addTarget(targets, std::move(target), error))
      return false;
  }
  return true;
}

bool parseHexID(StringRef text, uint64_t &value) {
  return text.size() == 18 && text.starts_with("0x") &&
         to_integer(text.drop_front(2), value, 16);
}

std::optional<uint32_t> parseKind(StringRef text, bool allowVoid) {
  if (text == "i64")
    return HFValueKindSignedInteger;
  if (text == "i1")
    return HFValueKindBool;
  if (text == "f32")
    return HFValueKindFloat32;
  if (text == "f64")
    return HFValueKindFloat64;
  if (text == "object")
    return HFValueKindHostHandle;
  if (text == "string")
    return HFValueKindStringHandle;
  if (allowVoid && text == "void")
    return HFValueKindVoid;
  return std::nullopt;
}

bool mergeManifest(StringRef inputPath, std::map<uint64_t, Target> &targets,
                   std::string &error) {
  auto buffer = MemoryBuffer::getFile(inputPath);
  if (!buffer) {
    error = "cannot read target manifest '" + inputPath.str() + "': " +
            buffer.getError().message();
    return false;
  }
  Expected<json::Value> parsed = json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    error = "cannot parse target manifest '" + inputPath.str() + "': " +
            toString(parsed.takeError());
    return false;
  }
  const json::Object *root = parsed->getAsObject();
  if (root == nullptr ||
      root->getInteger("schemaVersion") != ManifestSchemaVersion ||
      root->getInteger("abiVersion") != HF_ABI_VERSION) {
    error = "target manifest '" + inputPath.str() +
            "' has an unsupported schema or ABI version";
    return false;
  }
  const json::Array *array = root->getArray("targets");
  if (array == nullptr) {
    error = "target manifest '" + inputPath.str() +
            "' has no targets array";
    return false;
  }

  for (const json::Value &value : *array) {
    const json::Object *object = value.getAsObject();
    if (object == nullptr) {
      error = "target manifest '" + inputPath.str() +
              "' contains a non-object target";
      return false;
    }
    std::optional<StringRef> symbol = object->getString("symbol");
    std::optional<StringRef> targetID = object->getString("targetID");
    std::optional<StringRef> signatureID = object->getString("signatureID");
    std::optional<StringRef> returnName = object->getString("returnKind");
    std::optional<bool> hasReceiver = object->getBoolean("hasReceiver");
    std::optional<StringRef> receiverName = object->getString("receiverKind");
    const json::Array *argumentNames = object->getArray("argumentKinds");
    if (!symbol || !targetID || !signatureID || !returnName || !hasReceiver ||
        argumentNames == nullptr) {
      error = "target manifest '" + inputPath.str() +
              "' contains an incomplete target";
      return false;
    }

    Target target;
    target.symbol = symbol->str();
    if (!parseHexID(*targetID, target.targetID) ||
        !parseHexID(*signatureID, target.signatureID)) {
      error = "target '" + target.symbol + "' has an invalid hexadecimal ID";
      return false;
    }
    std::optional<uint32_t> parsedReturn = parseKind(*returnName, true);
    if (!parsedReturn) {
      error = "target '" + target.symbol + "' has an invalid return kind";
      return false;
    }
    target.returnKind = *parsedReturn;
    target.hasReceiver = *hasReceiver;
    if (receiverName) {
      const auto parsedReceiver = parseReceiverKind(*receiverName);
      if (!parsedReceiver) {
        error = "target '" + target.symbol + "' has an invalid receiver kind";
        return false;
      }
      target.receiverKind = *parsedReceiver;
    } else {
      target.receiverKind =
          target.hasReceiver ? ReceiverKind::Object : ReceiverKind::None;
    }
    if (target.hasReceiver != (target.receiverKind != ReceiverKind::None)) {
      error = "target '" + target.symbol +
              "' has inconsistent hasReceiver and receiverKind";
      return false;
    }
    for (const json::Value &argumentName : *argumentNames) {
      std::optional<StringRef> name = argumentName.getAsString();
      std::optional<uint32_t> kind =
          name ? parseKind(*name, false) : std::nullopt;
      if (!kind) {
        error = "target '" + target.symbol +
                "' has an invalid argument kind";
        return false;
      }
      target.argumentKinds.push_back(*kind);
    }
    if (!addTarget(targets, std::move(target), error))
      return false;
  }
  return true;
}

bool writeManifest(StringRef outputPath,
                   const std::map<uint64_t, Target> &targetMap,
                   std::string &error) {
  std::vector<const Target *> targets;
  targets.reserve(targetMap.size());
  for (const auto &entry : targetMap)
    targets.push_back(&entry.second);
  llvm::sort(targets, [](const Target *left, const Target *right) {
    if (left->symbol != right->symbol)
      return left->symbol < right->symbol;
    return left->targetID < right->targetID;
  });

  std::error_code fileError;
  raw_fd_ostream output(outputPath, fileError);
  if (fileError) {
    error = "cannot open output manifest '" + outputPath.str() + "': " +
            fileError.message();
    return false;
  }

  json::OStream json(output, 2);
  json.object([&] {
    json.attribute("schemaVersion", int64_t(ManifestSchemaVersion));
    json.attribute("abiVersion", int64_t(HF_ABI_VERSION));
    json.attributeArray("targets", [&] {
      for (const Target *target : targets) {
        json.object([&] {
          json.attribute("symbol", target->symbol);
          json.attribute("targetID", hexID(target->targetID));
          json.attribute("signatureID", hexID(target->signatureID));
          json.attribute("returnKind", *abiName(target->returnKind, true));
          json.attributeArray("argumentKinds", [&] {
            for (uint32_t kind : target->argumentKinds)
              json.value(*abiName(kind, false));
          });
          json.attribute("hasReceiver", target->hasReceiver);
          json.attribute("receiverKind",
                         receiverKindName(target->receiverKind));
        });
      }
    });
  });
  output << '\n';
  output.flush();
  if (output.has_error()) {
    error = "cannot write output manifest '" + outputPath.str() + "'";
    return false;
  }
  return true;
}

int fail(StringRef message) {
  errs() << "[HotfixManifest] error: " << message << '\n';
  return 1;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 3)
    return fail("usage: HotfixManifestTool <extract|merge> <output-json> "
                "[input ...]");

  StringRef mode = argv[1];
  StringRef outputPath = argv[2];
  std::map<uint64_t, Target> targets;
  std::string error;

  if (mode == "extract") {
    if (argc != 4)
      return fail("extract requires exactly one LLVM IR or bitcode input");
    if (!extractManifest(argv[3], targets, error))
      return fail(error);
  } else if (mode == "merge") {
    for (int index = 3; index < argc; ++index) {
      if (!mergeManifest(argv[index], targets, error))
        return fail(error);
    }
  } else {
    return fail("unknown command '" + mode.str() + "'");
  }

  if (!writeManifest(outputPath, targets, error))
    return fail(error);
  return 0;
}
