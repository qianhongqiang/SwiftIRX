#ifndef IRHotfixSDK_HFIR_h
#define IRHotfixSDK_HFIR_h

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace irhotfix::hfir {

inline constexpr std::uint16_t kVersion = 3;
inline constexpr std::size_t kMaximumHostArgumentCount = 16;
inline constexpr std::size_t kMaximumTargetArgumentCount = 8;
inline constexpr std::uint32_t kNoRegister =
    std::numeric_limits<std::uint32_t>::max();

enum class ValueType : std::uint8_t {
  Void = 0,
  Bool = 1,
  I64 = 2,
  F64 = 3,
  Handle = 4,
  String = 5,
  Bytes = 6,
  Point = 7,
  Size = 8,
  Rect = 9,
};

// Public target ABI kinds are intentionally separate from VM value types.
// In particular, Swift Float and Double both execute as F64 inside the VM,
// while their external calling conventions must remain distinguishable.
enum class TargetValueKind : std::uint8_t {
  Void = 0,
  Bool = 1,
  I64 = 2,
  F32 = 3,
  F64 = 4,
  Object = 5,
  String = 6,
};

enum class TargetReceiverKind : std::uint8_t {
  None = 0,
  Object = 1,
  Native = 2,
};

struct TargetABISchema {
  TargetValueKind returnType = TargetValueKind::Void;
  std::vector<TargetValueKind> parameterTypes;
  TargetReceiverKind receiverKind = TargetReceiverKind::None;
};

enum class ConstantKind : std::uint8_t {
  Bool = 1,
  I64 = 2,
  F64 = 3,
  String = 4,
  Bytes = 5,
  Point = 6,
  Size = 7,
  Rect = 8,
  NullHandle = 9,
};

enum class HostImportKind : std::uint8_t {
  Class = 1,
  Constructor = 2,
  Method = 3,
  Service = 4,
  NativeC = 5,
  NativeSwift = 6,
  NativeCXX = 7,
};

enum class OperandKind : std::uint8_t {
  Register = 1,
  Constant = 2,
  Block = 3,
  Import = 4,
  Function = 5,
  Local = 6,
};

enum class Opcode : std::uint16_t {
  Nop = 0,
  Constant = 1,
  Move = 2,
  Phi = 3,

  AddI64 = 10,
  SubI64 = 11,
  MulI64 = 12,
  DivI64 = 13,
  AddF64 = 14,
  SubF64 = 15,
  MulF64 = 16,
  DivF64 = 17,
  RemI64 = 18,
  UDivI64 = 19,

  CompareEqual = 20,
  CompareNotEqual = 21,
  CompareLessThan = 22,
  CompareLessEqual = 23,
  CompareGreaterThan = 24,
  CompareGreaterEqual = 25,
  Select = 26,

  Branch = 30,
  ConditionalBranch = 31,
  Return = 32,
  Trap = 33,
  Switch = 34,

  LocalAllocate = 40,
  LocalLoad = 41,
  LocalStore = 42,

  ObjectClass = 50,
  ObjectConstruct = 51,
  ObjectInvoke = 52,
  ObjectRelease = 53,
  StringConstant = 54,
  FunctionCall = 55,
  HostCall = 56,
  StringConcat = 57,
  PackRect = 58,

  TruncateI64 = 60,
  SignExtendI64 = 61,
  ZeroExtendI64 = 62,
  SignedIntToF64 = 63,
  UnsignedIntToF64 = 64,
  F64ToSignedInt = 65,
  F64ToUnsignedInt = 66,

  AndI64 = 70,
  OrI64 = 71,
  XorI64 = 72,
  ShiftLeftI64 = 73,
  ShiftRightSignedI64 = 74,
  ShiftRightUnsignedI64 = 75,
};

struct Constant {
  ConstantKind kind = ConstantKind::Bytes;
  std::uint64_t bits = 0;
  std::vector<std::uint8_t> bytes;
};

struct HostImport {
  std::uint64_t id = 0;
  HostImportKind kind = HostImportKind::Service;
  std::string owner;
  std::string name;
  std::string typeEncoding;
  ValueType returnType = ValueType::Void;
  std::vector<ValueType> parameterTypes;
  bool hasReceiver = false;
};

struct Operand {
  OperandKind kind = OperandKind::Register;
  ValueType type = ValueType::Void;
  std::uint32_t index = 0;
};

struct Instruction {
  Opcode opcode = Opcode::Nop;
  std::uint32_t result = kNoRegister;
  ValueType resultType = ValueType::Void;
  std::vector<Operand> operands;
};

struct BasicBlock {
  std::uint32_t id = 0;
  std::vector<Instruction> instructions;
};

struct Function {
  std::string name;
  ValueType returnType = ValueType::Void;
  std::vector<ValueType> parameterTypes;
  std::vector<ValueType> registerTypes;
  std::vector<ValueType> localTypes;
  std::uint32_t entryBlock = 0;
  std::vector<BasicBlock> blocks;
};

struct TargetDescriptor {
  std::uint64_t targetID = 0;
  std::uint64_t signatureID = 0;
  std::uint32_t entryFunction = 0;
  TargetABISchema abi;
};

struct DebugLocation {
  std::uint32_t function = 0;
  std::uint32_t block = 0;
  std::uint32_t instruction = 0;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string file;
};

struct Signature {
  std::string algorithm;
  std::string keyID;
  std::vector<std::uint8_t> bytes;
};

struct Package {
  std::uint32_t abiVersion = 3;
  std::string patchID;
  TargetDescriptor target;
  std::vector<Constant> constants;
  std::vector<HostImport> imports;
  std::vector<Function> functions;
  std::vector<DebugLocation> debugLocations;
  Signature signature;
};

const char *valueTypeName(ValueType type);
const char *targetValueKindName(TargetValueKind type);
const char *targetReceiverKindName(TargetReceiverKind kind);
const char *constantKindName(ConstantKind kind);
const char *hostImportKindName(HostImportKind kind);
const char *operandKindName(OperandKind kind);
const char *opcodeName(Opcode opcode);

bool isValidValueType(ValueType type);
bool isValidTargetValueKind(TargetValueKind type, bool allowVoid);
bool targetValueType(TargetValueKind type, ValueType &valueType);
std::uint64_t targetSignatureID(const TargetABISchema &schema);
bool isTerminator(Opcode opcode);

// Validates the stable HFIR contract independently of the binary container.
// On failure, error contains a deterministic path to the rejected field.
bool verify(const Package &package, std::string &error);

} // namespace irhotfix::hfir

#endif /* IRHotfixSDK_HFIR_h */
