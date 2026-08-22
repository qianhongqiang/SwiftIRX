#include "HFPatchContainer.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace irhotfix;

namespace {

[[noreturn]] void failed(const std::string &message) {
  std::cerr << "HFIRFormatTests: " << message << '\n';
  std::exit(1);
}

void require(bool condition, const std::string &message) {
  if (!condition)
    failed(message);
}

hfir::Package makePackage() {
  using hfir::ConstantKind;
  using hfir::Opcode;
  using hfir::Operand;
  using hfir::OperandKind;
  using hfir::ValueType;

  hfir::Package package;
  package.abiVersion = 1;
  package.patchID = "test.add-ten";
  package.target = {0x0102030405060708ULL, 0x8877665544332211ULL, 0};
  package.constants = {{ConstantKind::I64, 10, {}}};

  hfir::Function function;
  function.name = "addTen";
  function.returnType = ValueType::I64;
  function.parameterTypes = {ValueType::I64};
  function.registerTypes = {ValueType::I64, ValueType::I64, ValueType::I64};
  function.entryBlock = 7;
  function.blocks = {{
      7,
      {
          {Opcode::Constant, 1, ValueType::I64,
           {Operand{OperandKind::Constant, ValueType::I64, 0}}},
          {Opcode::AddI64, 2, ValueType::I64,
           {Operand{OperandKind::Register, ValueType::I64, 0},
            Operand{OperandKind::Register, ValueType::I64, 1}}},
          {Opcode::Return, hfir::kNoRegister, ValueType::Void,
           {Operand{OperandKind::Register, ValueType::I64, 2}}},
      },
  }};
  package.functions = {std::move(function)};
  package.debugLocations = {{0, 7, 1, 12, 5, "Patch.swift"}};
  package.signature = {"ed25519", "test-key", {1, 2, 3, 4}};
  return package;
}

} // namespace

int main() {
  std::string error;
  const hfir::Package source = makePackage();
  require(hfir::verify(source, error), "valid package rejected: " + error);

  std::vector<std::uint8_t> first;
  std::vector<std::uint8_t> second;
  require(container::encode(source, first, error), "encode failed: " + error);
  require(container::encode(source, second, error),
          "second encode failed: " + error);
  require(first == second, "encoding is not deterministic");

  hfir::Package decoded;
  require(container::decode(first, decoded, error), "decode failed: " + error);
  require(decoded.patchID == source.patchID, "patch ID did not round-trip");
  require(decoded.target.targetID == source.target.targetID,
          "target ID did not round-trip");
  require(decoded.functions.size() == 1 &&
              decoded.functions[0].blocks[0].instructions.size() == 3,
          "function body did not round-trip");
  require(decoded.signature.bytes == source.signature.bytes,
          "signature section did not round-trip");
  const std::string text = container::dump(decoded);
  require(text.find("add.i64 %0:i64 %1:i64") != std::string::npos,
          "dump omitted typed arithmetic instruction");

  hfir::Package duplicateDefinition = makePackage();
  duplicateDefinition.functions[0].blocks[0].instructions[1].result = 1;
  require(!hfir::verify(duplicateDefinition, error) &&
              error.find("more than one definition") != std::string::npos,
          "duplicate register definition was accepted");

  hfir::Package useBeforeDefinition = makePackage();
  auto &instructions = useBeforeDefinition.functions[0].blocks[0].instructions;
  std::swap(instructions[0], instructions[1]);
  require(!hfir::verify(useBeforeDefinition, error) &&
              error.find("does not dominate") != std::string::npos,
          "use-before-definition was accepted");

  std::vector<std::uint8_t> corrupted = first;
  corrupted[80] ^= 0x01;
  require(!container::decode(corrupted, decoded, error) &&
              error.find("integrity hash") != std::string::npos,
          "corrupted container was accepted");

  std::vector<std::uint8_t> truncated(first.begin(), first.end() - 1);
  require(!container::decode(truncated, decoded, error),
          "truncated container was accepted");

  std::cout << "HFIRFormatTests passed\n";
  return 0;
}
