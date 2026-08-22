#include "HFIR.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace irhotfix::hfir {
namespace {

ValueType constantType(ConstantKind kind) {
  switch (kind) {
  case ConstantKind::Bool:
    return ValueType::Bool;
  case ConstantKind::I64:
    return ValueType::I64;
  case ConstantKind::F64:
    return ValueType::F64;
  case ConstantKind::String:
    return ValueType::String;
  case ConstantKind::Bytes:
    return ValueType::Bytes;
  case ConstantKind::Point:
    return ValueType::Point;
  case ConstantKind::Size:
    return ValueType::Size;
  case ConstantKind::Rect:
    return ValueType::Rect;
  case ConstantKind::NullHandle:
    return ValueType::Handle;
  }
  return ValueType::Void;
}

bool fail(std::string &error, const std::string &path,
          const std::string &message) {
  error = path + ": " + message;
  return false;
}

bool isScalarArithmeticType(ValueType type) {
  return type == ValueType::I64 || type == ValueType::F64;
}

bool verifyType(ValueType type, bool allowVoid, std::string &error,
                const std::string &path) {
  if (!isValidValueType(type) || (!allowVoid && type == ValueType::Void))
    return fail(error, path, "invalid value type");
  return true;
}

bool verifyOperandReference(const Package &package, const Function &function,
                            const std::set<std::uint32_t> &blockIDs,
                            const Operand &operand, std::string &error,
                            const std::string &path) {
  switch (operand.kind) {
  case OperandKind::Register:
    if (operand.index >= function.registerTypes.size())
      return fail(error, path, "register index is out of range");
    if (operand.type != function.registerTypes[operand.index])
      return fail(error, path, "register operand type does not match register table");
    return true;
  case OperandKind::Constant:
    if (operand.index >= package.constants.size())
      return fail(error, path, "constant index is out of range");
    if (operand.type != constantType(package.constants[operand.index].kind))
      return fail(error, path, "constant operand type does not match constant table");
    return true;
  case OperandKind::Block:
    if (!blockIDs.contains(operand.index))
      return fail(error, path, "block ID does not exist in the function");
    if (operand.type != ValueType::Void)
      return fail(error, path, "block operands must have void type");
    return true;
  case OperandKind::Import:
    if (operand.index >= package.imports.size())
      return fail(error, path, "host import index is out of range");
    if (operand.type != ValueType::Void)
      return fail(error, path, "import operands must have void type");
    return true;
  case OperandKind::Function:
    if (operand.index >= package.functions.size())
      return fail(error, path, "function index is out of range");
    if (operand.type != ValueType::Void)
      return fail(error, path, "function operands must have void type");
    return true;
  case OperandKind::Local:
    if (operand.index >= function.localTypes.size())
      return fail(error, path, "local index is out of range");
    if (operand.type != function.localTypes[operand.index])
      return fail(error, path, "local operand type does not match local table");
    return true;
  }
  return fail(error, path, "unknown operand kind");
}

bool requireOperandKinds(const Instruction &instruction,
                         std::initializer_list<OperandKind> kinds,
                         std::string &error, const std::string &path) {
  if (instruction.operands.size() != kinds.size())
    return fail(error, path, "unexpected operand count");
  std::size_t index = 0;
  for (OperandKind kind : kinds) {
    if (instruction.operands[index].kind != kind)
      return fail(error, path + ".operands[" + std::to_string(index) + "]",
                  "unexpected operand kind");
    ++index;
  }
  return true;
}

bool requireResult(const Function &function, const Instruction &instruction,
                   ValueType expected, std::string &error,
                   const std::string &path) {
  if (instruction.result == kNoRegister)
    return fail(error, path + ".result", "instruction requires a result register");
  if (instruction.result >= function.registerTypes.size())
    return fail(error, path + ".result", "result register is out of range");
  if (instruction.resultType != function.registerTypes[instruction.result])
    return fail(error, path + ".resultType",
                "result type does not match register table");
  if (expected != ValueType::Void && instruction.resultType != expected)
    return fail(error, path + ".resultType", "instruction result type is invalid");
  return true;
}

bool requireNoResult(const Instruction &instruction, std::string &error,
                     const std::string &path) {
  if (instruction.result != kNoRegister ||
      instruction.resultType != ValueType::Void) {
    return fail(error, path, "instruction must not define a result");
  }
  return true;
}

bool verifyCallArguments(const std::vector<ValueType> &parameterTypes,
                         bool hasReceiver, const Instruction &instruction,
                         std::size_t firstArgument, std::string &error,
                         const std::string &path) {
  const std::size_t expected = parameterTypes.size() + (hasReceiver ? 1 : 0);
  if (instruction.operands.size() != firstArgument + expected)
    return fail(error, path, "call argument count does not match descriptor");
  std::size_t cursor = firstArgument;
  if (hasReceiver) {
    const Operand &receiver = instruction.operands[cursor++];
    if (receiver.kind != OperandKind::Register ||
        receiver.type != ValueType::Handle) {
      return fail(error, path + ".operands[" +
                             std::to_string(cursor - 1) + "]",
                  "receiver must be a handle register");
    }
  }
  for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
    const Operand &argument = instruction.operands[cursor++];
    if (argument.kind != OperandKind::Register ||
        argument.type != parameterTypes[index]) {
      return fail(error, path + ".operands[" +
                             std::to_string(cursor - 1) + "]",
                  "argument type does not match descriptor");
    }
  }
  return true;
}

bool verifyInstructionSemantics(const Package &package,
                                const Function &function,
                                const Instruction &instruction,
                                std::string &error,
                                const std::string &path) {
  const auto binaryArithmetic = [&](ValueType type) {
    if (!requireResult(function, instruction, type, error, path) ||
        !requireOperandKinds(instruction,
                             {OperandKind::Register, OperandKind::Register},
                             error, path))
      return false;
    if (instruction.operands[0].type != type ||
        instruction.operands[1].type != type)
      return fail(error, path, "arithmetic operands have the wrong type");
    return true;
  };

  switch (instruction.opcode) {
  case Opcode::Nop:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction, {}, error, path);
  case Opcode::Constant: {
    if (!requireResult(function, instruction, ValueType::Void, error, path) ||
        !requireOperandKinds(instruction, {OperandKind::Constant}, error, path))
      return false;
    return instruction.resultType == instruction.operands[0].type ||
           fail(error, path, "constant result type does not match its operand");
  }
  case Opcode::StringConstant:
    return requireResult(function, instruction, ValueType::String, error, path) &&
           requireOperandKinds(instruction, {OperandKind::Constant}, error, path) &&
           (instruction.operands[0].type == ValueType::String ||
            fail(error, path, "string.constant requires a string constant"));
  case Opcode::Move:
    return requireResult(function, instruction, ValueType::Void, error, path) &&
           requireOperandKinds(instruction, {OperandKind::Register}, error, path) &&
           (instruction.resultType == instruction.operands[0].type ||
            fail(error, path, "move source and result types differ"));
  case Opcode::Phi:
    if (!requireResult(function, instruction, ValueType::Void, error, path) ||
        instruction.operands.empty() || instruction.operands.size() % 2 != 0)
      return fail(error, path,
                  "phi requires register/block operand pairs and a result");
    for (std::size_t index = 0; index < instruction.operands.size(); index += 2) {
      if (instruction.operands[index].kind != OperandKind::Register ||
          instruction.operands[index].type != instruction.resultType ||
          instruction.operands[index + 1].kind != OperandKind::Block)
        return fail(error, path,
                    "phi incoming value or predecessor has an invalid type");
    }
    return true;
  case Opcode::AddI64:
  case Opcode::SubI64:
  case Opcode::MulI64:
  case Opcode::DivI64:
    return binaryArithmetic(ValueType::I64);
  case Opcode::AddF64:
  case Opcode::SubF64:
  case Opcode::MulF64:
  case Opcode::DivF64:
    return binaryArithmetic(ValueType::F64);
  case Opcode::CompareEqual:
  case Opcode::CompareNotEqual:
  case Opcode::CompareLessThan:
  case Opcode::CompareLessEqual:
  case Opcode::CompareGreaterThan:
  case Opcode::CompareGreaterEqual:
    if (!requireResult(function, instruction, ValueType::Bool, error, path) ||
        !requireOperandKinds(instruction,
                             {OperandKind::Register, OperandKind::Register},
                             error, path))
      return false;
    return instruction.operands[0].type == instruction.operands[1].type &&
                   (isScalarArithmeticType(instruction.operands[0].type) ||
                    instruction.operands[0].type == ValueType::Bool ||
                    ((instruction.opcode == Opcode::CompareEqual ||
                      instruction.opcode == Opcode::CompareNotEqual) &&
                     instruction.operands[0].type == ValueType::Handle))
               ? true
               : fail(error, path,
                      "comparison requires two equal scalar operand types");
  case Opcode::Branch:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction, {OperandKind::Block}, error, path);
  case Opcode::ConditionalBranch:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction,
                               {OperandKind::Register, OperandKind::Block,
                                OperandKind::Block},
                               error, path) &&
           (instruction.operands[0].type == ValueType::Bool ||
            fail(error, path, "conditional branch condition must be bool"));
  case Opcode::Return:
    if (!requireNoResult(instruction, error, path))
      return false;
    if (function.returnType == ValueType::Void)
      return requireOperandKinds(instruction, {}, error, path);
    return requireOperandKinds(instruction, {OperandKind::Register}, error,
                               path) &&
           (instruction.operands[0].type == function.returnType ||
            fail(error, path, "return operand type does not match function"));
  case Opcode::Trap:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction, {}, error, path);
  case Opcode::LocalAllocate:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction, {OperandKind::Local}, error, path);
  case Opcode::LocalLoad:
    return requireResult(function, instruction, ValueType::Void, error, path) &&
           requireOperandKinds(instruction, {OperandKind::Local}, error, path) &&
           (instruction.resultType == instruction.operands[0].type ||
            fail(error, path, "local.load result type does not match local"));
  case Opcode::LocalStore:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction,
                               {OperandKind::Local, OperandKind::Register},
                               error, path) &&
           (instruction.operands[0].type == instruction.operands[1].type ||
            fail(error, path, "local.store operand types differ"));
  case Opcode::ObjectClass: {
    if (!requireResult(function, instruction, ValueType::Handle, error, path) ||
        !requireOperandKinds(instruction, {OperandKind::Import}, error, path))
      return false;
    const HostImport &import = package.imports[instruction.operands[0].index];
    return import.kind == HostImportKind::Class ||
           fail(error, path, "object.class requires a class import");
  }
  case Opcode::ObjectConstruct: {
    if (!requireResult(function, instruction, ValueType::Handle, error, path) ||
        instruction.operands.empty() ||
        instruction.operands[0].kind != OperandKind::Import)
      return fail(error, path, "object.construct requires a constructor import");
    const HostImport &import = package.imports[instruction.operands[0].index];
    return import.kind == HostImportKind::Constructor &&
                   verifyCallArguments(import.parameterTypes, import.hasReceiver,
                                       instruction, 1, error, path)
               ? true
               : (error.empty()
                      ? fail(error, path,
                             "object.construct requires a constructor import")
                      : false);
  }
  case Opcode::ObjectInvoke: {
    if (instruction.operands.empty() ||
        instruction.operands[0].kind != OperandKind::Import)
      return fail(error, path, "object.invoke requires a method or service import");
    const HostImport &import = package.imports[instruction.operands[0].index];
    if (import.kind != HostImportKind::Method &&
        import.kind != HostImportKind::Service)
      return fail(error, path, "object.invoke requires a method or service import");
    if (import.returnType == ValueType::Void) {
      if (!requireNoResult(instruction, error, path))
        return false;
    } else if (!requireResult(function, instruction, import.returnType, error,
                              path)) {
      return false;
    }
    return verifyCallArguments(import.parameterTypes, import.hasReceiver,
                               instruction, 1, error, path);
  }
  case Opcode::ObjectRelease:
    return requireNoResult(instruction, error, path) &&
           requireOperandKinds(instruction, {OperandKind::Register}, error,
                               path) &&
           (instruction.operands[0].type == ValueType::Handle ||
            fail(error, path, "object.release requires a handle"));
  case Opcode::FunctionCall: {
    if (instruction.operands.empty() ||
        instruction.operands[0].kind != OperandKind::Function)
      return fail(error, path, "function.call requires a function operand");
    const Function &callee = package.functions[instruction.operands[0].index];
    if (callee.returnType == ValueType::Void) {
      if (!requireNoResult(instruction, error, path))
        return false;
    } else if (!requireResult(function, instruction, callee.returnType, error,
                              path)) {
      return false;
    }
    return verifyCallArguments(callee.parameterTypes, false, instruction, 1,
                               error, path);
  }
  }
  return fail(error, path, "unknown opcode");
}

} // namespace

const char *valueTypeName(ValueType type) {
  switch (type) {
  case ValueType::Void: return "void";
  case ValueType::Bool: return "bool";
  case ValueType::I64: return "i64";
  case ValueType::F64: return "f64";
  case ValueType::Handle: return "handle";
  case ValueType::String: return "string";
  case ValueType::Bytes: return "bytes";
  case ValueType::Point: return "point";
  case ValueType::Size: return "size";
  case ValueType::Rect: return "rect";
  }
  return "invalid";
}

const char *constantKindName(ConstantKind kind) {
  switch (kind) {
  case ConstantKind::Bool: return "bool";
  case ConstantKind::I64: return "i64";
  case ConstantKind::F64: return "f64";
  case ConstantKind::String: return "string";
  case ConstantKind::Bytes: return "bytes";
  case ConstantKind::Point: return "point";
  case ConstantKind::Size: return "size";
  case ConstantKind::Rect: return "rect";
  case ConstantKind::NullHandle: return "null-handle";
  }
  return "invalid";
}

const char *hostImportKindName(HostImportKind kind) {
  switch (kind) {
  case HostImportKind::Class: return "class";
  case HostImportKind::Constructor: return "constructor";
  case HostImportKind::Method: return "method";
  case HostImportKind::Service: return "service";
  }
  return "invalid";
}

const char *operandKindName(OperandKind kind) {
  switch (kind) {
  case OperandKind::Register: return "register";
  case OperandKind::Constant: return "constant";
  case OperandKind::Block: return "block";
  case OperandKind::Import: return "import";
  case OperandKind::Function: return "function";
  case OperandKind::Local: return "local";
  }
  return "invalid";
}

const char *opcodeName(Opcode opcode) {
  switch (opcode) {
  case Opcode::Nop: return "nop";
  case Opcode::Constant: return "const";
  case Opcode::Move: return "move";
  case Opcode::Phi: return "phi";
  case Opcode::AddI64: return "add.i64";
  case Opcode::SubI64: return "sub.i64";
  case Opcode::MulI64: return "mul.i64";
  case Opcode::DivI64: return "div.i64";
  case Opcode::AddF64: return "add.f64";
  case Opcode::SubF64: return "sub.f64";
  case Opcode::MulF64: return "mul.f64";
  case Opcode::DivF64: return "div.f64";
  case Opcode::CompareEqual: return "compare.eq";
  case Opcode::CompareNotEqual: return "compare.ne";
  case Opcode::CompareLessThan: return "compare.lt";
  case Opcode::CompareLessEqual: return "compare.le";
  case Opcode::CompareGreaterThan: return "compare.gt";
  case Opcode::CompareGreaterEqual: return "compare.ge";
  case Opcode::Branch: return "branch";
  case Opcode::ConditionalBranch: return "branch.conditional";
  case Opcode::Return: return "return";
  case Opcode::Trap: return "trap";
  case Opcode::LocalAllocate: return "local.alloc";
  case Opcode::LocalLoad: return "local.load";
  case Opcode::LocalStore: return "local.store";
  case Opcode::ObjectClass: return "object.class";
  case Opcode::ObjectConstruct: return "object.construct";
  case Opcode::ObjectInvoke: return "object.invoke";
  case Opcode::ObjectRelease: return "object.release";
  case Opcode::StringConstant: return "string.constant";
  case Opcode::FunctionCall: return "function.call";
  }
  return "invalid";
}

bool isValidValueType(ValueType type) {
  return static_cast<std::uint8_t>(type) <=
         static_cast<std::uint8_t>(ValueType::Rect);
}

bool isTerminator(Opcode opcode) {
  return opcode == Opcode::Branch || opcode == Opcode::ConditionalBranch ||
         opcode == Opcode::Return || opcode == Opcode::Trap;
}

bool verify(const Package &package, std::string &error) {
  error.clear();
  if (package.abiVersion == 0)
    return fail(error, "package.abiVersion", "must not be zero");
  if (package.patchID.empty())
    return fail(error, "package.patchID", "must not be empty");
  if (package.target.targetID == 0 || package.target.signatureID == 0)
    return fail(error, "package.target", "target and signature IDs must not be zero");
  if (package.functions.empty())
    return fail(error, "package.functions", "must contain at least one function");
  if (package.target.entryFunction >= package.functions.size())
    return fail(error, "package.target.entryFunction", "is out of range");

  for (std::size_t index = 0; index < package.constants.size(); ++index) {
    const Constant &constant = package.constants[index];
    const std::string path = "package.constants[" + std::to_string(index) + "]";
    if (constantType(constant.kind) == ValueType::Void)
      return fail(error, path + ".kind", "invalid constant kind");
    const bool isBytes = constant.kind == ConstantKind::String ||
                         constant.kind == ConstantKind::Bytes ||
                         constant.kind == ConstantKind::Point ||
                         constant.kind == ConstantKind::Size ||
                         constant.kind == ConstantKind::Rect;
    if (!isBytes && !constant.bytes.empty())
      return fail(error, path + ".bytes", "scalar constants cannot contain bytes");
    if (constant.kind == ConstantKind::Bool && constant.bits > 1)
      return fail(error, path + ".bits", "bool constant must be zero or one");
    if (constant.kind == ConstantKind::NullHandle &&
        (constant.bits != 0 || !constant.bytes.empty()))
      return fail(error, path, "null handle constant must have an empty payload");
    const std::size_t requiredAggregateSize =
        constant.kind == ConstantKind::Point || constant.kind == ConstantKind::Size
            ? 16
            : (constant.kind == ConstantKind::Rect ? 32 : 0);
    if (requiredAggregateSize != 0 &&
        constant.bytes.size() != requiredAggregateSize)
      return fail(error, path + ".bytes",
                  "aggregate constant has an invalid canonical byte count");
  }

  std::set<std::uint64_t> importIDs;
  for (std::size_t index = 0; index < package.imports.size(); ++index) {
    const HostImport &import = package.imports[index];
    const std::string path = "package.imports[" + std::to_string(index) + "]";
    if (import.id == 0 || !importIDs.insert(import.id).second)
      return fail(error, path + ".id", "must be nonzero and unique");
    if (static_cast<std::uint8_t>(import.kind) <
            static_cast<std::uint8_t>(HostImportKind::Class) ||
        static_cast<std::uint8_t>(import.kind) >
            static_cast<std::uint8_t>(HostImportKind::Service))
      return fail(error, path + ".kind", "invalid host import kind");
    if (import.name.empty())
      return fail(error, path + ".name", "must not be empty");
    if (import.kind != HostImportKind::Service && import.owner.empty())
      return fail(error, path + ".owner", "must not be empty");
    if (!verifyType(import.returnType, true, error, path + ".returnType"))
      return false;
    for (std::size_t parameter = 0;
         parameter < import.parameterTypes.size(); ++parameter) {
      if (!verifyType(import.parameterTypes[parameter], false, error,
                      path + ".parameterTypes[" +
                          std::to_string(parameter) + "]"))
        return false;
    }
    if (import.kind == HostImportKind::Class &&
        (import.returnType != ValueType::Handle || import.hasReceiver ||
         !import.parameterTypes.empty())) {
      return fail(error, path, "class import must return a handle without arguments");
    }
  }

  std::set<std::string> functionNames;
  for (std::size_t functionIndex = 0;
       functionIndex < package.functions.size(); ++functionIndex) {
    const Function &function = package.functions[functionIndex];
    const std::string functionPath =
        "package.functions[" + std::to_string(functionIndex) + "]";
    if (function.name.empty() || !functionNames.insert(function.name).second)
      return fail(error, functionPath + ".name", "must be nonempty and unique");
    if (!verifyType(function.returnType, true, error,
                    functionPath + ".returnType"))
      return false;
    if (function.registerTypes.size() < function.parameterTypes.size())
      return fail(error, functionPath + ".registerTypes",
                  "must begin with all parameter registers");
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
      if (!verifyType(function.parameterTypes[index], false, error,
                      functionPath + ".parameterTypes[" +
                          std::to_string(index) + "]"))
        return false;
      if (function.registerTypes[index] != function.parameterTypes[index])
        return fail(error, functionPath + ".registerTypes[" +
                               std::to_string(index) + "]",
                    "parameter registers must be the register table prefix");
    }
    for (std::size_t index = 0; index < function.registerTypes.size(); ++index) {
      if (!verifyType(function.registerTypes[index], false, error,
                      functionPath + ".registerTypes[" +
                          std::to_string(index) + "]"))
        return false;
    }
    for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
      if (!verifyType(function.localTypes[index], false, error,
                      functionPath + ".localTypes[" +
                          std::to_string(index) + "]"))
        return false;
    }
    if (function.blocks.empty())
      return fail(error, functionPath + ".blocks", "must not be empty");

    std::set<std::uint32_t> blockIDs;
    for (const BasicBlock &block : function.blocks) {
      if (!blockIDs.insert(block.id).second)
        return fail(error, functionPath + ".blocks",
                    "block IDs must be unique");
    }
    if (!blockIDs.contains(function.entryBlock))
      return fail(error, functionPath + ".entryBlock", "does not exist");

    std::vector<bool> registerDefined(function.registerTypes.size(), false);
    std::vector<std::size_t> definitionBlock(function.registerTypes.size(), 0);
    std::vector<std::size_t> definitionInstruction(function.registerTypes.size(), 0);
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index)
      registerDefined[index] = true;

    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
         ++blockIndex) {
      const BasicBlock &block = function.blocks[blockIndex];
      const std::string blockPath = functionPath + ".blocks[" +
                                    std::to_string(blockIndex) + "]";
      if (block.instructions.empty())
        return fail(error, blockPath + ".instructions", "must not be empty");
      for (std::size_t instructionIndex = 0;
           instructionIndex < block.instructions.size(); ++instructionIndex) {
        const Instruction &instruction = block.instructions[instructionIndex];
        const std::string instructionPath =
            blockPath + ".instructions[" + std::to_string(instructionIndex) + "]";
        if (instruction.opcode == Opcode::Phi && instructionIndex != 0 &&
            block.instructions[instructionIndex - 1].opcode != Opcode::Phi)
          return fail(error, instructionPath,
                      "phi instructions must form the basic block prefix");
        for (std::size_t operandIndex = 0;
             operandIndex < instruction.operands.size(); ++operandIndex) {
          if (!verifyOperandReference(
                  package, function, blockIDs, instruction.operands[operandIndex],
                  error, instructionPath + ".operands[" +
                             std::to_string(operandIndex) + "]"))
            return false;
        }
        if (!verifyInstructionSemantics(package, function, instruction, error,
                                        instructionPath))
          return false;
        if (instruction.result != kNoRegister) {
          if (registerDefined[instruction.result])
            return fail(error, instructionPath + ".result",
                        "register has more than one definition");
          registerDefined[instruction.result] = true;
          definitionBlock[instruction.result] = blockIndex;
          definitionInstruction[instruction.result] = instructionIndex;
        }
        const bool last = instructionIndex + 1 == block.instructions.size();
        if (isTerminator(instruction.opcode) != last)
          return fail(error, instructionPath,
                      last ? "basic block must end with a terminator"
                           : "terminator must be the final instruction");
      }
    }

    for (std::size_t index = 0; index < registerDefined.size(); ++index) {
      if (!registerDefined[index])
        return fail(error, functionPath + ".registerTypes[" +
                               std::to_string(index) + "]",
                    "register has no definition");
    }

    std::map<std::uint32_t, std::size_t> blockIndexByID;
    for (std::size_t index = 0; index < function.blocks.size(); ++index)
      blockIndexByID.emplace(function.blocks[index].id, index);
    const std::size_t entryIndex = blockIndexByID.at(function.entryBlock);
    std::vector<std::vector<std::size_t>> successors(function.blocks.size());
    std::vector<std::vector<std::size_t>> predecessors(function.blocks.size());
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
      const Instruction &terminator = function.blocks[index].instructions.back();
      if (terminator.opcode == Opcode::Branch) {
        successors[index].push_back(blockIndexByID.at(terminator.operands[0].index));
      } else if (terminator.opcode == Opcode::ConditionalBranch) {
        successors[index].push_back(blockIndexByID.at(terminator.operands[1].index));
        successors[index].push_back(blockIndexByID.at(terminator.operands[2].index));
      }
      for (std::size_t successor : successors[index])
        predecessors[successor].push_back(index);
    }

    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
         ++blockIndex) {
      const BasicBlock &block = function.blocks[blockIndex];
      for (std::size_t instructionIndex = 0;
           instructionIndex < block.instructions.size(); ++instructionIndex) {
        const Instruction &instruction = block.instructions[instructionIndex];
        if (instruction.opcode != Opcode::Phi)
          break;
        for (std::size_t operandIndex = 1;
             operandIndex < instruction.operands.size(); operandIndex += 2) {
          const std::size_t incomingBlock =
              blockIndexByID.at(instruction.operands[operandIndex].index);
          if (std::find(predecessors[blockIndex].begin(),
                        predecessors[blockIndex].end(), incomingBlock) ==
              predecessors[blockIndex].end()) {
            return fail(error,
                        functionPath + ".blocks[" +
                            std::to_string(blockIndex) + "].instructions[" +
                            std::to_string(instructionIndex) + "].operands[" +
                            std::to_string(operandIndex) + "]",
                        "phi incoming block is not a CFG predecessor");
          }
        }
      }
    }

    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<std::size_t> worklist = {entryIndex};
    while (!worklist.empty()) {
      const std::size_t current = worklist.back();
      worklist.pop_back();
      if (reachable[current])
        continue;
      reachable[current] = true;
      worklist.insert(worklist.end(), successors[current].begin(),
                      successors[current].end());
    }
    for (std::size_t index = 0; index < reachable.size(); ++index) {
      if (!reachable[index])
        return fail(error, functionPath + ".blocks[" + std::to_string(index) +
                               "]",
                    "block is unreachable from the entry block");
    }

    std::set<std::size_t> allBlocks;
    for (std::size_t index = 0; index < function.blocks.size(); ++index)
      allBlocks.insert(index);
    std::vector<std::set<std::size_t>> dominators(function.blocks.size(),
                                                   allBlocks);
    dominators[entryIndex] = {entryIndex};
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
           ++blockIndex) {
        if (blockIndex == entryIndex)
          continue;
        std::set<std::size_t> next = allBlocks;
        for (std::size_t predecessor : predecessors[blockIndex]) {
          std::set<std::size_t> intersection;
          std::set_intersection(next.begin(), next.end(),
                                dominators[predecessor].begin(),
                                dominators[predecessor].end(),
                                std::inserter(intersection, intersection.begin()));
          next = std::move(intersection);
        }
        next.insert(blockIndex);
        if (next != dominators[blockIndex]) {
          dominators[blockIndex] = std::move(next);
          changed = true;
        }
      }
    }

    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
         ++blockIndex) {
      const BasicBlock &block = function.blocks[blockIndex];
      for (std::size_t instructionIndex = 0;
           instructionIndex < block.instructions.size(); ++instructionIndex) {
        const Instruction &instruction = block.instructions[instructionIndex];
        for (std::size_t operandIndex = 0;
             operandIndex < instruction.operands.size(); ++operandIndex) {
          const Operand &operand = instruction.operands[operandIndex];
          if (operand.kind != OperandKind::Register ||
              operand.index < function.parameterTypes.size())
            continue;
          const std::size_t definingBlock = definitionBlock[operand.index];
          bool dominates = false;
          if (instruction.opcode == Opcode::Phi && operandIndex % 2 == 0) {
            const std::size_t predecessor = blockIndexByID.at(
                instruction.operands[operandIndex + 1].index);
            dominates = definingBlock == predecessor ||
                        dominators[predecessor].contains(definingBlock);
          } else {
            dominates = definingBlock == blockIndex
                            ? definitionInstruction[operand.index] <
                                  instructionIndex
                            : dominators[blockIndex].contains(definingBlock);
          }
          if (!dominates) {
            return fail(error,
                        functionPath + ".blocks[" +
                            std::to_string(blockIndex) + "].instructions[" +
                            std::to_string(instructionIndex) + "].operands[" +
                            std::to_string(operandIndex) + "]",
                        "register definition does not dominate its use");
          }
        }
      }
    }
  }

  for (std::size_t index = 0; index < package.debugLocations.size(); ++index) {
    const DebugLocation &location = package.debugLocations[index];
    const std::string path =
        "package.debugLocations[" + std::to_string(index) + "]";
    if (location.function >= package.functions.size())
      return fail(error, path + ".function", "is out of range");
    const Function &function = package.functions[location.function];
    const auto block = std::find_if(
        function.blocks.begin(), function.blocks.end(),
        [&](const BasicBlock &candidate) { return candidate.id == location.block; });
    if (block == function.blocks.end())
      return fail(error, path + ".block", "does not exist");
    if (location.instruction >= block->instructions.size())
      return fail(error, path + ".instruction", "is out of range");
  }
  if ((!package.signature.algorithm.empty() || !package.signature.keyID.empty()) &&
      package.signature.bytes.empty())
    return fail(error, "package.signature.bytes", "must not be empty");
  if (!package.signature.bytes.empty() &&
      (package.signature.algorithm.empty() || package.signature.keyID.empty()))
    return fail(error, "package.signature",
                "signed package requires algorithm and key ID");
  return true;
}

} // namespace irhotfix::hfir
