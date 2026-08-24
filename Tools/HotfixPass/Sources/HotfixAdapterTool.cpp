#include "HFHostAdapter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace llvm;

namespace {
constexpr uint32_t ManifestSchemaVersion = 1;

struct Adapter {
  std::string language;
  std::string symbol;
  std::string owner;
  std::string callKind;
  std::string returnKind;
  std::vector<std::string> argumentKinds;
  std::string binding;
  std::string receiverType;
  std::string header;
  bool mainThreadOnly = false;
  bool objcCompatibleHandles = false;
  bool noSideEffects = false;
};

uint64_t appendFNV(uint64_t hash, StringRef bytes) {
  for (unsigned char byte : bytes.bytes()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t appendU32(uint64_t hash, uint32_t value) {
  char bytes[4] = {
      static_cast<char>(value), static_cast<char>(value >> 8),
      static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  return appendFNV(hash, StringRef(bytes, sizeof(bytes)));
}

uint64_t callID(StringRef symbol) {
  return appendFNV(14695981039346656037ULL, symbol);
}

std::optional<uint32_t> languageValue(StringRef value) {
  if (value == "swift")
    return HFHostLanguageSwift;
  if (value == "c")
    return HFHostLanguageC;
  if (value == "cxx")
    return HFHostLanguageCXX;
  return std::nullopt;
}

std::optional<uint32_t> callKindValue(StringRef value) {
  if (value == "function")
    return HFHostCallKindFunction;
  if (value == "instanceMethod")
    return HFHostCallKindInstanceMethod;
  if (value == "staticMethod")
    return HFHostCallKindStaticMethod;
  return std::nullopt;
}

std::optional<uint32_t> valueKind(StringRef value, bool allowVoid) {
  if (value == "i64")
    return HFValueKindSignedInteger;
  if (value == "i1")
    return HFValueKindBool;
  if (value == "void" && allowVoid)
    return HFValueKindVoid;
  return std::nullopt;
}

uint32_t flags(const Adapter &adapter) {
  uint32_t result = 0;
  if (adapter.callKind == "instanceMethod")
    result |= HFHostCallFlagHasReceiver;
  if (adapter.mainThreadOnly)
    result |= HFHostCallFlagMainThreadOnly;
  if (adapter.objcCompatibleHandles)
    result |= HFHostCallFlagObjCCompatibleHandles;
  if (adapter.noSideEffects)
    result |= HFHostCallFlagNoSideEffects;
  return result;
}

uint64_t signatureID(const Adapter &adapter) {
  uint64_t hash = 14695981039346656037ULL;
  hash = appendU32(hash, *languageValue(adapter.language));
  hash = appendU32(hash, *callKindValue(adapter.callKind));
  hash = appendU32(hash, *valueKind(adapter.returnKind, true));
  hash = appendU32(hash, flags(adapter) & HFHostCallFlagHasReceiver);
  hash = appendU32(hash, static_cast<uint32_t>(adapter.argumentKinds.size()));
  for (const std::string &kind : adapter.argumentKinds)
    hash = appendU32(hash, *valueKind(kind, false));
  return hash;
}

std::string hexID(uint64_t value) {
  return "0x" + utohexstr(value, true, 16);
}

bool validQualifiedName(StringRef value, bool allowColon) {
  if (value.empty())
    return false;
  for (char character : value) {
    if (std::isalnum(static_cast<unsigned char>(character)) ||
        character == '_' || character == '.' ||
        (allowColon && character == ':'))
      continue;
    return false;
  }
  return true;
}

bool parseBool(const json::Object &object, StringRef key, bool &output,
               std::string &error) {
  const json::Value *value = object.get(key);
  if (value == nullptr)
    return true;
  const std::optional<bool> boolean = value->getAsBoolean();
  if (!boolean) {
    error = "adapter field '" + key.str() + "' must be a boolean";
    return false;
  }
  output = *boolean;
  return true;
}

bool parseRequiredString(const json::Object &object, StringRef key,
                         std::string &output, std::string &error) {
  const std::optional<StringRef> value = object.getString(key);
  if (!value || value->empty()) {
    error = "adapter field '" + key.str() + "' must be a non-empty string";
    return false;
  }
  output = value->str();
  return true;
}

bool parseOptionalString(const json::Object &object, StringRef key,
                         std::string &output, std::string &error) {
  const json::Value *value = object.get(key);
  if (value == nullptr)
    return true;
  const std::optional<StringRef> string = value->getAsString();
  if (!string) {
    error = "adapter field '" + key.str() + "' must be a string";
    return false;
  }
  output = string->str();
  return true;
}

bool parseAdapter(const json::Object &object, Adapter &adapter,
                  std::string &error) {
  if (!parseRequiredString(object, "language", adapter.language, error) ||
      !parseRequiredString(object, "symbol", adapter.symbol, error) ||
      !parseRequiredString(object, "callKind", adapter.callKind, error) ||
      !parseRequiredString(object, "returnKind", adapter.returnKind, error) ||
      !parseRequiredString(object, "binding", adapter.binding, error) ||
      !parseOptionalString(object, "owner", adapter.owner, error) ||
      !parseOptionalString(object, "receiverType", adapter.receiverType,
                           error) ||
      !parseOptionalString(object, "header", adapter.header, error) ||
      !parseBool(object, "mainThreadOnly", adapter.mainThreadOnly, error) ||
      !parseBool(object, "objcCompatibleHandles",
                 adapter.objcCompatibleHandles, error) ||
      !parseBool(object, "noSideEffects", adapter.noSideEffects, error))
    return false;

  if (!languageValue(adapter.language)) {
    error = "adapter '" + adapter.symbol +
            "' uses unsupported language '" + adapter.language + "'";
    return false;
  }
  if (!callKindValue(adapter.callKind)) {
    error = "adapter '" + adapter.symbol +
            "' uses unsupported call kind '" + adapter.callKind + "'";
    return false;
  }
  if (!valueKind(adapter.returnKind, true)) {
    error = "adapter '" + adapter.symbol + "' has unsupported return kind";
    return false;
  }

  const json::Array *arguments = object.getArray("argumentKinds");
  if (arguments == nullptr) {
    error = "adapter '" + adapter.symbol +
            "' must contain an argumentKinds array";
    return false;
  }
  if (arguments->size() > HF_MAX_HOST_ARGUMENT_COUNT) {
    error = "adapter '" + adapter.symbol + "' has too many arguments";
    return false;
  }
  for (const json::Value &value : *arguments) {
    const std::optional<StringRef> kind = value.getAsString();
    if (!kind || !valueKind(*kind, false)) {
      error = "adapter '" + adapter.symbol +
              "' has an unsupported argument kind";
      return false;
    }
    adapter.argumentKinds.push_back(kind->str());
  }

  const bool instance = adapter.callKind == "instanceMethod";
  if (instance != !adapter.receiverType.empty()) {
    error = "adapter '" + adapter.symbol +
            "' must specify receiverType exactly for instanceMethod";
    return false;
  }
  if (adapter.language == "c" && adapter.callKind != "function") {
    error = "adapter '" + adapter.symbol + "' uses an invalid C call kind";
    return false;
  }
  if (adapter.language == "cxx" && adapter.callKind == "staticMethod") {
    error = "adapter '" + adapter.symbol +
            "' must describe a C++ static method as a function";
    return false;
  }
  if (adapter.language != "swift" &&
      (adapter.mainThreadOnly || adapter.objcCompatibleHandles)) {
    error = "adapter '" + adapter.symbol +
            "' uses flags unsupported by the generated C/C++ gateway";
    return false;
  }
  if (adapter.language != "swift" && adapter.header.empty()) {
    error = "adapter '" + adapter.symbol +
            "' must specify a declaration header";
    return false;
  }
  if (adapter.header.find_first_of("\r\n\"") != std::string::npos) {
    error = "adapter '" + adapter.symbol + "' has an invalid header path";
    return false;
  }
  if (!validQualifiedName(adapter.binding, adapter.language != "swift") ||
      (!adapter.receiverType.empty() &&
       !validQualifiedName(adapter.receiverType,
                           adapter.language != "swift"))) {
    error = "adapter '" + adapter.symbol + "' has an invalid binding name";
    return false;
  }
  return true;
}

bool loadSpec(StringRef path, std::vector<Adapter> &adapters,
              std::string &error) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer = MemoryBuffer::getFile(path);
  if (!buffer) {
    error = "cannot read adapter specification '" + path.str() + "': " +
            buffer.getError().message();
    return false;
  }
  Expected<json::Value> parsed = json::parse((*buffer)->getBuffer());
  if (!parsed) {
    error = "cannot parse adapter specification '" + path.str() + "': " +
            toString(parsed.takeError());
    return false;
  }
  json::Object *root = parsed->getAsObject();
  if (root == nullptr || root->getInteger("schemaVersion") !=
                             ManifestSchemaVersion) {
    error = "adapter specification uses an unsupported schema version";
    return false;
  }
  json::Array *entries = root->getArray("adapters");
  if (entries == nullptr) {
    error = "adapter specification must contain an adapters array";
    return false;
  }
  for (const json::Value &value : *entries) {
    const json::Object *object = value.getAsObject();
    if (object == nullptr) {
      error = "every adapter entry must be an object";
      return false;
    }
    Adapter adapter;
    if (!parseAdapter(*object, adapter, error))
      return false;
    for (const Adapter &existing : adapters) {
      if (existing.symbol == adapter.symbol) {
        error = "duplicate adapter symbol '" + adapter.symbol + "'";
        return false;
      }
      if (callID(existing.symbol) == callID(adapter.symbol)) {
        error = "adapter import ID collision between '" + existing.symbol +
                "' and '" + adapter.symbol + "'";
        return false;
      }
    }
    adapters.push_back(std::move(adapter));
  }
  return true;
}

std::string swiftString(StringRef value) {
  std::string output = "\"";
  for (char character : value) {
    switch (character) {
    case '\\': output += "\\\\"; break;
    case '"': output += "\\\""; break;
    case '\n': output += "\\n"; break;
    case '\r': output += "\\r"; break;
    case '\t': output += "\\t"; break;
    default: output += character; break;
    }
  }
  output += '"';
  return output;
}

std::string cxxString(StringRef value) {
  return swiftString(value);
}

StringRef cxxKind(StringRef kind) {
  if (kind == "i64")
    return "HFValueKindSignedInteger";
  if (kind == "i1")
    return "HFValueKindBool";
  return "HFValueKindVoid";
}

StringRef swiftKind(StringRef kind) {
  if (kind == "i64")
    return "HFValueKind(HFValueKindSignedInteger)";
  if (kind == "i1")
    return "HFValueKind(HFValueKindBool)";
  return "HFValueKind(HFValueKindVoid)";
}

std::string swiftDecode(StringRef kind, size_t index) {
  if (kind == "i64")
    return "Int(bitPattern: UInt(arguments[" + std::to_string(index) +
           "].bits))";
  return "arguments[" + std::to_string(index) + "].bits != 0";
}

std::string swiftEncode(StringRef kind, StringRef expression) {
  if (kind == "i64")
    return "HFMakeValue(HFValueKind(HFValueKindSignedInteger), "
           "UInt64(bitPattern: Int64(" + expression.str() + ")))";
  if (kind == "i1")
    return "HFMakeValue(HFValueKind(HFValueKindBool), " + expression.str() +
           " ? 1 : 0)";
  return "HFMakeValue(HFValueKind(HFValueKindVoid), 0)";
}

std::string renderSwift(const std::vector<Adapter> &adapters) {
  std::string output;
  raw_string_ostream stream(output);
  stream << "// Generated by HotfixAdapterTool. Do not edit.\n"
            "import Foundation\n\n"
            "private nonisolated enum HotfixGeneratedAdapterError: Error {\n"
            "    case invalidReceiver\n"
            "}\n\n"
            "nonisolated enum HotfixGeneratedHostAdapters {\n"
            "    private static let lock = NSLock()\n"
            "    private static var registrations: "
            "[HotfixHostAdapterRegistration]?\n\n"
            "    static func registerAll() -> HFStatus {\n"
            "        lock.lock()\n"
            "        defer { lock.unlock() }\n"
            "        if registrations != nil { return HFStatus(HFStatusApplied) }\n"
            "        var created: [HotfixHostAdapterRegistration] = []\n"
            "        do {\n";

  bool hasNative = false;
  for (const Adapter &adapter : adapters) {
    if (adapter.language != "swift") {
      hasNative = true;
      continue;
    }
    stream << "            created.append(try HotfixSwiftHostAdapter.register(\n"
              "                HotfixSwiftHostCall(\n"
              "                    symbol: " << swiftString(adapter.symbol)
           << ",\n"
              "                    owner: " << swiftString(adapter.owner) << ",\n"
              "                    callKind: HFHostCallKind(";
    stream << (adapter.callKind == "function"
                   ? "HFHostCallKindFunction"
                   : adapter.callKind == "instanceMethod"
                         ? "HFHostCallKindInstanceMethod"
                         : "HFHostCallKindStaticMethod");
    stream << "),\n"
              "                    returnKind: "
           << swiftKind(adapter.returnKind) << ",\n"
              "                    argumentKinds: [";
    for (size_t index = 0; index < adapter.argumentKinds.size(); ++index) {
      if (index != 0)
        stream << ", ";
      stream << swiftKind(adapter.argumentKinds[index]);
    }
    stream << "],\n"
              "                    hasReceiver: "
           << (adapter.callKind == "instanceMethod" ? "true" : "false")
           << ",\n"
              "                    mainThreadOnly: "
           << (adapter.mainThreadOnly ? "true" : "false") << ",\n"
              "                    objcCompatibleHandles: "
           << (adapter.objcCompatibleHandles ? "true" : "false") << ",\n"
              "                    noSideEffects: "
           << (adapter.noSideEffects ? "true" : "false")
           << "\n                )\n"
              "            ) { receiver, arguments in\n";
    if (adapter.callKind == "instanceMethod") {
      stream << "                guard receiver.token != 0,\n"
                "                      let pointer = UnsafeRawPointer(bitPattern: "
                "UInt(receiver.token)),\n"
                "                      let object = Unmanaged<AnyObject>"
                ".fromOpaque(pointer).takeUnretainedValue()\n"
                "                          as? "
             << adapter.receiverType
             << " else { throw HotfixGeneratedAdapterError.invalidReceiver }\n";
    }
    stream << "                ";
    if (adapter.returnKind != "void")
      stream << "let value = ";
    if (adapter.callKind == "instanceMethod")
      stream << "object." << adapter.binding;
    else
      stream << adapter.binding;
    stream << '(';
    for (size_t index = 0; index < adapter.argumentKinds.size(); ++index) {
      if (index != 0)
        stream << ", ";
      stream << swiftDecode(adapter.argumentKinds[index], index);
    }
    stream << ")\n                return "
           << swiftEncode(adapter.returnKind, "value")
           << "\n            })\n";
  }
  if (hasNative) {
    stream << "            let nativeStatus = "
              "irhf_register_generated_native_adapters()\n"
              "            guard nativeStatus == HFStatus(HFStatusApplied) else "
              "{ return nativeStatus }\n";
  }
  stream << "            registrations = created\n"
            "            return HFStatus(HFStatusApplied)\n"
            "        } catch let HotfixHostAdapterError.registrationFailed(status) {\n"
            "            return status\n"
            "        } catch {\n"
            "            return HFStatus(HFStatusExecutionFailed)\n"
            "        }\n"
            "    }\n"
            "}\n";
  stream.flush();
  return output;
}

std::string renderObjCXX(const std::vector<Adapter> &adapters) {
  std::string output;
  raw_string_ostream stream(output);
  stream << "// Generated by HotfixAdapterTool. Do not edit.\n"
            "#include \"../HFGeneratedHostAdapters.h\"\n"
            "#include \"../HFHostAdapter.hpp\"\n\n"
            "#include <mutex>\n"
            "#include <utility>\n"
            "#include <vector>\n";
  std::vector<std::string> headers;
  for (const Adapter &adapter : adapters) {
    if (adapter.language == "swift")
      continue;
    bool seen = false;
    for (const std::string &header : headers)
      seen |= header == adapter.header;
    if (!seen) {
      headers.push_back(adapter.header);
      stream << "#include \"" << adapter.header << "\"\n";
    }
  }
  stream << "\nextern \"C\" HFStatus "
            "irhf_register_generated_native_adapters(void) {\n"
            "  static std::mutex mutex;\n"
            "  static std::vector<irhotfix::host::Registration> registrations;\n"
            "  std::lock_guard<std::mutex> guard(mutex);\n"
            "  if (!registrations.empty())\n"
            "    return HFStatusApplied;\n"
            "  std::vector<irhotfix::host::Registration> created;\n";
  for (const Adapter &adapter : adapters) {
    if (adapter.language == "swift")
      continue;
    stream << "  {\n"
              "    irhotfix::host::Registration registration;\n"
              "    const HFStatus status = ";
    if (adapter.callKind == "instanceMethod") {
      stream << "irhotfix::host::registerMethod(" << cxxString(adapter.symbol)
             << ", &" << adapter.binding << ", registration, "
             << (adapter.noSideEffects ? "true" : "false") << ");\n";
    } else {
      stream << "irhotfix::host::registerFunction(" << cxxString(adapter.symbol)
             << ", "
             << (adapter.language == "c" ? "HFHostLanguageC"
                                          : "HFHostLanguageCXX")
             << ", &" << adapter.binding << ", registration, "
             << (adapter.noSideEffects ? "true" : "false") << ");\n";
    }
    stream << "    if (status != HFStatusApplied)\n"
              "      return status;\n"
              "    static constexpr std::array<HFValueKind, "
           << adapter.argumentKinds.size() << "> expectedKinds = {";
    for (size_t argumentIndex = 0;
         argumentIndex < adapter.argumentKinds.size(); ++argumentIndex) {
      if (argumentIndex != 0)
        stream << ", ";
      stream << cxxKind(adapter.argumentKinds[argumentIndex]);
    }
    stream << "};\n"
              "    HFHostCallDescriptor expected = irhotfix::host::descriptor(\n"
              "        "
           << cxxString(adapter.symbol) << ", "
           << (adapter.language == "c" ? "HFHostLanguageC"
                                        : "HFHostLanguageCXX")
           << ", "
           << (adapter.callKind == "instanceMethod"
                   ? "HFHostCallKindInstanceMethod"
                   : "HFHostCallKindFunction")
           << ", " << cxxKind(adapter.returnKind)
           << ", expectedKinds.data(), expectedKinds.size(), "
           << (adapter.callKind == "instanceMethod" ? "true" : "false")
           << ", " << cxxString(adapter.owner) << ", "
           << (adapter.noSideEffects ? "true" : "false") << ");\n"
              "    const HFStatus validationStatus = "
              "hf_host_adapter_validate(&expected);\n"
              "    if (validationStatus != HFStatusApplied)\n"
              "      return validationStatus;\n"
              "    created.push_back(std::move(registration));\n"
              "  }\n";
  }
  stream << "  registrations = std::move(created);\n"
            "  return HFStatusApplied;\n"
            "}\n";
  stream.flush();
  return output;
}

std::string renderManifest(const std::vector<Adapter> &adapters) {
  json::Array entries;
  for (const Adapter &adapter : adapters) {
    json::Array arguments;
    for (const std::string &kind : adapter.argumentKinds)
      arguments.push_back(kind);
    entries.push_back(json::Object{
        {"language", adapter.language},
        {"symbol", adapter.symbol},
        {"importID", hexID(callID(adapter.symbol))},
        {"signatureID", hexID(signatureID(adapter))},
        {"owner", adapter.owner},
        {"callKind", adapter.callKind},
        {"returnKind", adapter.returnKind},
        {"argumentKinds", std::move(arguments)},
        {"hasReceiver", adapter.callKind == "instanceMethod"},
        {"mainThreadOnly", adapter.mainThreadOnly},
        {"objcCompatibleHandles", adapter.objcCompatibleHandles},
        {"noSideEffects", adapter.noSideEffects},
    });
  }
  std::string output;
  raw_string_ostream stream(output);
  stream << formatv("{0:2}\n", json::Value(json::Object{
                                   {"schemaVersion", ManifestSchemaVersion},
                                   {"abiVersion", HF_HOST_ADAPTER_ABI_VERSION},
                                   {"adapters", std::move(entries)},
                               }));
  stream.flush();
  return output;
}

bool writeIfChanged(StringRef path, StringRef contents, std::string &error) {
  if (ErrorOr<std::unique_ptr<MemoryBuffer>> existing =
          MemoryBuffer::getFile(path)) {
    if ((*existing)->getBuffer() == contents)
      return true;
  }
  std::error_code code;
  raw_fd_ostream output(path, code);
  if (code) {
    error = "cannot write '" + path.str() + "': " + code.message();
    return false;
  }
  output << contents;
  output.close();
  if (output.has_error()) {
    error = "cannot finish writing '" + path.str() + "'";
    return false;
  }
  return true;
}

int generate(int argc, char **argv) {
  if (argc != 6) {
    errs() << "usage: HotfixAdapterTool generate <spec.json> "
              "<generated.swift> <generated.mm> <manifest.json>\n";
    return 64;
  }
  std::vector<Adapter> adapters;
  std::string error;
  if (!loadSpec(argv[2], adapters, error) ||
      !writeIfChanged(argv[3], renderSwift(adapters), error) ||
      !writeIfChanged(argv[4], renderObjCXX(adapters), error) ||
      !writeIfChanged(argv[5], renderManifest(adapters), error)) {
    errs() << "HotfixAdapterTool: error: " << error << '\n';
    return 1;
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  if (argc >= 2 && StringRef(argv[1]) == "generate")
    return generate(argc, argv);
  errs() << "usage: HotfixAdapterTool generate <spec.json> <generated.swift> "
            "<generated.mm> <manifest.json>\n";
  return 64;
}
