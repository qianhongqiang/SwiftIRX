#include "HFPatchContainer.h"

#include <bit>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace irhotfix;

namespace {

std::uint64_t fnv1a64(const std::string &text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : text) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::vector<std::uint8_t> doubles(std::initializer_list<double> values) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(values.size() * sizeof(double));
  for (double value : values) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
  }
  return bytes;
}

hfir::Operand reg(std::uint32_t index, hfir::ValueType type) {
  return {hfir::OperandKind::Register, type, index};
}

hfir::Operand constant(std::uint32_t index, hfir::ValueType type) {
  return {hfir::OperandKind::Constant, type, index};
}

hfir::Operand hostImport(std::uint32_t index) {
  return {hfir::OperandKind::Import, hfir::ValueType::Void, index};
}

hfir::Instruction result(hfir::Opcode opcode, std::uint32_t destination,
                         hfir::ValueType type,
                         std::initializer_list<hfir::Operand> operands) {
  return {opcode, destination, type, operands};
}

hfir::Instruction effect(hfir::Opcode opcode,
                         std::initializer_list<hfir::Operand> operands) {
  return {opcode, hfir::kNoRegister, hfir::ValueType::Void, operands};
}

hfir::HostImport imported(hfir::HostImportKind kind, std::string owner,
                          std::string name, hfir::ValueType returnType,
                          std::vector<hfir::ValueType> parameters,
                          bool hasReceiver, std::string encoding = {}) {
  const std::string identity = owner + "." + name + ":" + encoding;
  return {fnv1a64(identity), kind, std::move(owner), std::move(name),
          std::move(encoding), returnType, std::move(parameters), hasReceiver};
}

hfir::Package setupUIExample() {
  using hfir::Constant;
  using hfir::ConstantKind;
  using hfir::HostImportKind;
  using hfir::Opcode;
  using hfir::ValueType;

  hfir::Package package;
  package.abiVersion = 1;
  package.patchID = "example.setup-ui.hfir-v1";
  package.target = {0x1232093bb65a3a2bULL, 0x3fc60f0529f431f8ULL, 0};
  package.constants = {
      Constant{ConstantKind::Rect, 0, doubles({0, 0, 100, 100})},
      Constant{ConstantKind::Rect, 0, doubles({10, 10, 80, 20})},
      Constant{ConstantKind::String, 0, {'h', 'e', 'l', 'l', 'o'}},
  };
  package.imports = {
      imported(HostImportKind::Class, "UIView", "class", ValueType::Handle,
               {}, false),
      imported(HostImportKind::Constructor, "UIView", "initWithFrame:",
               ValueType::Handle, {ValueType::Rect}, true,
               "@{CGRect={CGPoint=dd}{CGSize=dd}}"),
      imported(HostImportKind::Class, "UIColor", "class", ValueType::Handle,
               {}, false),
      imported(HostImportKind::Method, "UIColor", "yellowColor",
               ValueType::Handle, {}, true, "@@:"),
      imported(HostImportKind::Method, "UIView", "setBackgroundColor:",
               ValueType::Void, {ValueType::Handle}, true, "v@:@"),
      imported(HostImportKind::Class, "UILabel", "class", ValueType::Handle,
               {}, false),
      imported(HostImportKind::Constructor, "UILabel", "initWithFrame:",
               ValueType::Handle, {ValueType::Rect}, true,
               "@{CGRect={CGPoint=dd}{CGSize=dd}}"),
      imported(HostImportKind::Method, "UILabel", "setText:",
               ValueType::Void, {ValueType::String}, true, "v@:@"),
      imported(HostImportKind::Method, "UIViewController", "view",
               ValueType::Handle, {}, true, "@@:"),
      imported(HostImportKind::Method, "UIView", "addSubview:",
               ValueType::Void, {ValueType::Handle}, true, "v@:@"),
  };

  hfir::Function function;
  function.name = "setupUI";
  function.returnType = ValueType::Void;
  function.parameterTypes = {ValueType::Handle};
  function.registerTypes = {
      ValueType::Handle, // 0: self
      ValueType::Rect,   // 1: view frame
      ValueType::Handle, // 2: UIView class
      ValueType::Handle, // 3: view
      ValueType::Handle, // 4: UIColor class
      ValueType::Handle, // 5: yellow color
      ValueType::Rect,   // 6: label frame
      ValueType::Handle, // 7: UILabel class
      ValueType::Handle, // 8: label
      ValueType::String, // 9: hello
      ValueType::Handle, // 10: controller view
  };
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          result(Opcode::Constant, 1, ValueType::Rect,
                 {constant(0, ValueType::Rect)}),
          result(Opcode::ObjectClass, 2, ValueType::Handle, {hostImport(0)}),
          result(Opcode::ObjectConstruct, 3, ValueType::Handle,
                 {hostImport(1), reg(2, ValueType::Handle),
                  reg(1, ValueType::Rect)}),
          result(Opcode::ObjectClass, 4, ValueType::Handle, {hostImport(2)}),
          result(Opcode::ObjectInvoke, 5, ValueType::Handle,
                 {hostImport(3), reg(4, ValueType::Handle)}),
          effect(Opcode::ObjectInvoke,
                 {hostImport(4), reg(3, ValueType::Handle),
                  reg(5, ValueType::Handle)}),
          result(Opcode::Constant, 6, ValueType::Rect,
                 {constant(1, ValueType::Rect)}),
          result(Opcode::ObjectClass, 7, ValueType::Handle, {hostImport(5)}),
          result(Opcode::ObjectConstruct, 8, ValueType::Handle,
                 {hostImport(6), reg(7, ValueType::Handle),
                  reg(6, ValueType::Rect)}),
          result(Opcode::StringConstant, 9, ValueType::String,
                 {constant(2, ValueType::String)}),
          effect(Opcode::ObjectInvoke,
                 {hostImport(7), reg(8, ValueType::Handle),
                  reg(9, ValueType::String)}),
          effect(Opcode::ObjectInvoke,
                 {hostImport(9), reg(3, ValueType::Handle),
                  reg(8, ValueType::Handle)}),
          result(Opcode::ObjectInvoke, 10, ValueType::Handle,
                 {hostImport(8), reg(0, ValueType::Handle)}),
          effect(Opcode::ObjectInvoke,
                 {hostImport(9), reg(10, ValueType::Handle),
                  reg(3, ValueType::Handle)}),
          effect(Opcode::Return, {}),
      },
  }};
  package.functions = {std::move(function)};
  package.debugLocations = {{0, 0, 0, 3, 16, "ViewController.swift"},
                            {0, 0, 9, 6, 22, "ViewController.swift"}};
  return package;
}

int reportError(const std::string &message) {
  std::cerr << "[HotfixPackage] error: " << message << '\n';
  return 1;
}

bool loadPackage(const std::string &path, hfir::Package &package,
                 std::string &error) {
  std::vector<std::uint8_t> bytes;
  return container::readFile(path, bytes, error) &&
         container::decode(bytes, package, error);
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "create-example") {
    std::string error;
    std::vector<std::uint8_t> bytes;
    if (!container::encode(setupUIExample(), bytes, error) ||
        !container::writeFile(argv[2], bytes, error))
      return reportError(error);
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "verify") {
    std::string error;
    hfir::Package package;
    if (!loadPackage(argv[2], package, error))
      return reportError(error);
    std::cout << "verified " << argv[2] << " (patch " << package.patchID
              << ")\n";
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "dump") {
    std::string error;
    hfir::Package package;
    if (!loadPackage(argv[2], package, error))
      return reportError(error);
    std::cout << container::dump(package);
    return 0;
  }
  return reportError(
      "usage: HotfixPackageTool create-example <output.hfpatch>\n"
      "       HotfixPackageTool verify <input.hfpatch>\n"
      "       HotfixPackageTool dump <input.hfpatch>");
}
