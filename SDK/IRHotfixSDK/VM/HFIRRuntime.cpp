#include "HFIRRuntime.h"

#include "../Bridge/IRHotfixObjCBridge.h"
#include "../Format/HFPatchContainer.h"
#include "../HostAdapter/HFHostAdapter.h"
#include "../HostHandle/HFHostHandleTable.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace irhotfix;

constexpr std::uint64_t kInstructionBudget = 100'000;
constexpr std::uint32_t kMaximumCallDepth = 64;
static_assert(hfir::kMaximumHostArgumentCount == HF_MAX_HOST_ARGUMENT_COUNT);

struct HostHandleLifetime {
  HFHandle handle = HFInvalidHandle();
  HFHostHandleLease *lease = nullptr;
  bool ownsHandle = false;

  ~HostHandleLifetime() {
    hf_host_handle_lease_release(lease);
    if (ownsHandle && handle.token != 0)
      hf_host_handle_release(handle);
  }
};

struct VMValue {
  hfir::ValueType type = hfir::ValueType::Void;
  std::uint64_t bits = 0;
  void *object = nullptr;
  HFHandle handle = HFInvalidHandle();
  std::shared_ptr<HostHandleLifetime> lifetime;
  std::vector<std::uint8_t> bytes;
  bool initialized = false;

  static VMValue scalar(hfir::ValueType type, std::uint64_t bits) {
    VMValue value;
    value.type = type;
    value.bits = bits;
    value.initialized = true;
    return value;
  }

  static VMValue aggregate(hfir::ValueType type,
                           const std::vector<std::uint8_t> &bytes) {
    VMValue value;
    value.type = type;
    value.bytes = bytes;
    value.initialized = true;
    return value;
  }

  static bool hostObject(hfir::ValueType type, HFHandle handle,
                         bool ownsHandle, VMValue &value) {
    value = {};
    value.type = type;
    value.handle = handle;
    value.initialized = true;
    if (handle.token == 0) {
      value.object = nullptr;
      return true;
    }
    auto lifetime = std::make_shared<HostHandleLifetime>();
    lifetime->handle = handle;
    lifetime->ownsHandle = ownsHandle;
    if (hf_host_handle_resolve(handle, &lifetime->lease,
                               &value.object) != HFStatusApplied)
      return false;
    value.lifetime = std::move(lifetime);
    return true;
  }

  static bool adoptRetainedObject(hfir::ValueType type, void *object,
                                  HFHandleKind kind, VMValue &value) {
    if (object == nullptr)
      return hostObject(type, HFInvalidHandle(), false, value);
    HFHandle handle = HFInvalidHandle();
    const HFStatus status = hf_host_handle_register(
        object, kind, HFHostHandleOwnershipStrong, &handle);
    IRHFObjCReleaseRetainedObject(object);
    return status == HFStatusApplied && hostObject(type, handle, true, value);
  }
};

struct InstalledPatch {
  std::uint64_t token = 0;
  hfir::Package package;
};

class PatchRegistry {
public:
  static PatchRegistry &shared() {
    static PatchRegistry registry;
    return registry;
  }

  HFStatus install(const void *bytes, std::size_t byteCount,
                   HFIRPatchHandle *handle) {
    if (bytes == nullptr || byteCount == 0 || handle == nullptr)
      return HFStatusInvalidArguments;
    try {
      const auto *begin = static_cast<const std::uint8_t *>(bytes);
      std::vector<std::uint8_t> encoded(begin, begin + byteCount);
      hfir::Package package;
      std::string error;
      if (!container::decode(encoded, package, error))
        return HFStatusExecutionFailed;
      if (package.abiVersion != HF_ABI_VERSION)
        return HFStatusABIVersionMismatch;

      auto patch = std::make_shared<InstalledPatch>();
      patch->token = nextToken_.fetch_add(1, std::memory_order_relaxed);
      if (patch->token == 0)
        patch->token = nextToken_.fetch_add(1, std::memory_order_relaxed);
      patch->package = std::move(package);
      {
        std::lock_guard lock(mutex_);
        installed_[patch->token] = patch;
      }
      *handle = {patch->token, patch->package.target.targetID,
                 patch->package.target.signatureID};
      return HFStatusApplied;
    } catch (...) {
      return HFStatusExecutionFailed;
    }
  }

  HFStatus activate(HFIRPatchHandle handle) {
    std::lock_guard lock(mutex_);
    const auto found = installed_.find(handle.token);
    if (found == installed_.end() || !matches(handle, *found->second))
      return HFStatusNoPatch;
    active_[handle.targetID] = found->second;
    return HFStatusApplied;
  }

  HFStatus deactivate(HFIRPatchHandle handle) {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(handle.targetID);
    if (found == active_.end() || found->second->token != handle.token)
      return HFStatusNoPatch;
    active_.erase(found);
    return HFStatusApplied;
  }

  HFStatus uninstall(HFIRPatchHandle handle) {
    std::lock_guard lock(mutex_);
    const auto found = installed_.find(handle.token);
    if (found == installed_.end() || !matches(handle, *found->second))
      return HFStatusNoPatch;
    const auto active = active_.find(handle.targetID);
    if (active != active_.end() && active->second->token == handle.token)
      active_.erase(active);
    installed_.erase(found);
    return HFStatusApplied;
  }

  std::shared_ptr<const InstalledPatch> active(std::uint64_t targetID) {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(targetID);
    return found == active_.end() ? nullptr : found->second;
  }

private:
  static bool matches(HFIRPatchHandle handle, const InstalledPatch &patch) {
    return handle.targetID == patch.package.target.targetID &&
           handle.signatureID == patch.package.target.signatureID;
  }

  std::mutex mutex_;
  std::atomic<std::uint64_t> nextToken_{1};
  std::unordered_map<std::uint64_t, std::shared_ptr<InstalledPatch>> installed_;
  std::unordered_map<std::uint64_t, std::shared_ptr<InstalledPatch>> active_;
};

class Executor {
public:
  explicit Executor(const hfir::Package &package) : package_(package) {}
  ~Executor() {
    for (const auto &[importID, lease] : nativeLeases_)
      hf_host_adapter_release(lease);
  }

  bool invoke(const HFPatchFrame &frame, VMValue &result) {
    if (!preflightHostImports())
      return false;
    const hfir::Function &entry =
        package_.functions[package_.target.entryFunction];
    std::vector<VMValue> arguments;
    arguments.reserve(entry.parameterTypes.size());
    std::size_t parameter = 0;
    const bool hasReceiver =
        (frame.flags & HFPatchFrameFlagHasReceiver) != 0;
    if (hasReceiver) {
      if (entry.parameterTypes.empty() ||
          entry.parameterTypes.front() != hfir::ValueType::Handle ||
          frame.receiver.token == 0)
        return false;
      VMValue receiver;
      if (!VMValue::hostObject(hfir::ValueType::Handle, frame.receiver, false,
                               receiver))
        return false;
      arguments.push_back(std::move(receiver));
      parameter = 1;
    }
    if (entry.parameterTypes.size() - parameter != frame.argumentCount)
      return false;
    for (std::uint32_t index = 0; index < frame.argumentCount; ++index) {
      const hfir::ValueType expected = entry.parameterTypes[parameter + index];
      const HFValue &source = frame.arguments[index];
      VMValue decoded;
      if (!decodeFrameValue(source, expected, decoded))
        return false;
      arguments.push_back(std::move(decoded));
    }
    std::uint64_t budget = kInstructionBudget;
    return executeFunction(package_.target.entryFunction, arguments, result,
                           budget, 0);
  }

  bool hostEffectsStarted() const { return hostEffectsStarted_; }

private:
  static bool decodeFrameValue(const HFValue &source, hfir::ValueType expected,
                               VMValue &value) {
    if (source.flags != HFValueFlagNone)
      return false;
    switch (expected) {
    case hfir::ValueType::Bool:
      if (source.kind != HFValueKindBool || source.bits > 1)
        return false;
      value = VMValue::scalar(expected, source.bits);
      return true;
    case hfir::ValueType::I64:
      if (source.kind != HFValueKindSignedInteger &&
          source.kind != HFValueKindUnsignedInteger)
        return false;
      value = VMValue::scalar(expected, source.bits);
      return true;
    case hfir::ValueType::F64:
      if (source.kind != HFValueKindFloat64)
        return false;
      value = VMValue::scalar(expected, source.bits);
      return true;
    default:
      return false;
    }
  }

  static bool writeFrameResult(const VMValue &source, HFValue &result) {
    result = HFMakeValue(HFValueKindInvalid, 0);
    switch (source.type) {
    case hfir::ValueType::Void:
      result = HFMakeValue(HFValueKindVoid, 0);
      return true;
    case hfir::ValueType::Bool:
      result = HFMakeValue(HFValueKindBool, source.bits);
      return source.bits <= 1;
    case hfir::ValueType::I64:
      result = HFMakeValue(HFValueKindSignedInteger, source.bits);
      return true;
    case hfir::ValueType::F64:
      result = HFMakeValue(HFValueKindFloat64, source.bits);
      return true;
    default:
      return false;
    }
  }

  bool executeFunction(std::uint32_t functionIndex,
                       const std::vector<VMValue> &arguments, VMValue &result,
                       std::uint64_t &budget, std::uint32_t depth) {
    if (depth >= kMaximumCallDepth || functionIndex >= package_.functions.size())
      return false;
    const hfir::Function &function = package_.functions[functionIndex];
    if (arguments.size() != function.parameterTypes.size())
      return false;
    std::vector<VMValue> registers(function.registerTypes.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (!arguments[index].initialized ||
          arguments[index].type != function.parameterTypes[index])
        return false;
      registers[index] = arguments[index];
    }
    std::vector<VMValue> locals(function.localTypes.size());
    std::unordered_map<std::uint32_t, const hfir::BasicBlock *> blocks;
    for (const hfir::BasicBlock &block : function.blocks)
      blocks.emplace(block.id, &block);

    std::uint32_t current = function.entryBlock;
    std::optional<std::uint32_t> predecessor;
    for (;;) {
      const auto blockFound = blocks.find(current);
      if (blockFound == blocks.end())
        return false;
      const hfir::BasicBlock &block = *blockFound->second;
      bool transferred = false;
      for (const hfir::Instruction &instruction : block.instructions) {
        if (budget-- == 0)
          return false;
        std::vector<VMValue> constantOperands(instruction.operands.size());
        auto operand = [&](std::size_t index) -> const VMValue * {
          if (index >= instruction.operands.size())
            return nullptr;
          const hfir::Operand &source = instruction.operands[index];
          if (source.kind == hfir::OperandKind::Register) {
            if (source.index >= registers.size())
              return nullptr;
            const VMValue &value = registers[source.index];
            return value.initialized ? &value : nullptr;
          }
          if (source.kind != hfir::OperandKind::Constant ||
              source.index >= package_.constants.size())
            return nullptr;
          const hfir::Constant &constant = package_.constants[source.index];
          VMValue &value = constantOperands[index];
          switch (constant.kind) {
          case hfir::ConstantKind::Bool:
            value = VMValue::scalar(hfir::ValueType::Bool, constant.bits);
            break;
          case hfir::ConstantKind::I64:
            value = VMValue::scalar(hfir::ValueType::I64, constant.bits);
            break;
          case hfir::ConstantKind::F64:
            value = VMValue::scalar(hfir::ValueType::F64, constant.bits);
            break;
          case hfir::ConstantKind::NullHandle:
            if (!VMValue::hostObject(hfir::ValueType::Handle,
                                     HFInvalidHandle(), false, value))
              return nullptr;
            break;
          default:
            return nullptr;
          }
          return &value;
        };
        auto assign = [&](VMValue value) {
          if (instruction.result == hfir::kNoRegister ||
              instruction.result >= registers.size() || !value.initialized ||
              value.type != instruction.resultType)
            return false;
          registers[instruction.result] = std::move(value);
          return true;
        };

        switch (instruction.opcode) {
        case hfir::Opcode::Nop:
        case hfir::Opcode::LocalAllocate:
          break;
        case hfir::Opcode::Constant:
        case hfir::Opcode::StringConstant: {
          const hfir::Constant &constant =
              package_.constants[instruction.operands[0].index];
          VMValue value;
          switch (constant.kind) {
          case hfir::ConstantKind::Bool:
            value = VMValue::scalar(hfir::ValueType::Bool, constant.bits);
            break;
          case hfir::ConstantKind::I64:
            value = VMValue::scalar(hfir::ValueType::I64, constant.bits);
            break;
          case hfir::ConstantKind::F64:
            value = VMValue::scalar(hfir::ValueType::F64, constant.bits);
            break;
          case hfir::ConstantKind::String: {
            void *string = IRHFObjCCreateStringUTF8(
                constant.bytes.data(), constant.bytes.size());
            if (string == nullptr)
              return false;
            if (!VMValue::adoptRetainedObject(
                    hfir::ValueType::String, string, HFHandleKindObject,
                    value))
              return false;
            value.bytes = constant.bytes;
            break;
          }
          case hfir::ConstantKind::Bytes:
            value = VMValue::aggregate(hfir::ValueType::Bytes, constant.bytes);
            break;
          case hfir::ConstantKind::Point:
            value = VMValue::aggregate(hfir::ValueType::Point, constant.bytes);
            break;
          case hfir::ConstantKind::Size:
            value = VMValue::aggregate(hfir::ValueType::Size, constant.bytes);
            break;
          case hfir::ConstantKind::Rect:
            value = VMValue::aggregate(hfir::ValueType::Rect, constant.bytes);
            break;
          case hfir::ConstantKind::NullHandle:
            if (!VMValue::hostObject(hfir::ValueType::Handle,
                                     HFInvalidHandle(), false, value))
              return false;
            break;
          }
          if (!assign(std::move(value)))
            return false;
          break;
        }
        case hfir::Opcode::Move: {
          const VMValue *source = operand(0);
          if (source == nullptr || !assign(*source))
            return false;
          break;
        }
        case hfir::Opcode::Select: {
          const VMValue *condition = operand(0);
          const VMValue *selected =
              condition == nullptr || condition->type != hfir::ValueType::Bool
                  ? nullptr
                  : operand(condition->bits ? 1 : 2);
          if (selected == nullptr || !assign(*selected))
            return false;
          break;
        }
        case hfir::Opcode::Phi: {
          if (!predecessor)
            return false;
          const VMValue *selected = nullptr;
          for (std::size_t index = 0; index < instruction.operands.size();
               index += 2) {
            if (instruction.operands[index + 1].index == *predecessor) {
              selected = operand(index);
              break;
            }
          }
          if (selected == nullptr || !assign(*selected))
            return false;
          break;
        }
        case hfir::Opcode::AddI64:
        case hfir::Opcode::SubI64:
        case hfir::Opcode::MulI64:
        case hfir::Opcode::DivI64:
        case hfir::Opcode::RemI64:
        case hfir::Opcode::UDivI64:
        case hfir::Opcode::AndI64:
        case hfir::Opcode::OrI64:
        case hfir::Opcode::XorI64:
        case hfir::Opcode::ShiftLeftI64:
        case hfir::Opcode::ShiftRightSignedI64:
        case hfir::Opcode::ShiftRightUnsignedI64: {
          const VMValue *left = operand(0);
          const VMValue *right = operand(1);
          if (left == nullptr || right == nullptr)
            return false;
          std::uint64_t bits = 0;
          if (instruction.opcode == hfir::Opcode::AddI64)
            bits = left->bits + right->bits;
          else if (instruction.opcode == hfir::Opcode::SubI64)
            bits = left->bits - right->bits;
          else if (instruction.opcode == hfir::Opcode::MulI64)
            bits = left->bits * right->bits;
          else if (instruction.opcode == hfir::Opcode::DivI64 ||
                   instruction.opcode == hfir::Opcode::RemI64) {
            const auto divisor = static_cast<std::int64_t>(right->bits);
            const auto dividend = static_cast<std::int64_t>(left->bits);
            if (divisor == 0 ||
                (dividend == std::numeric_limits<std::int64_t>::min() &&
                 divisor == -1))
              return false;
            bits = static_cast<std::uint64_t>(
                instruction.opcode == hfir::Opcode::DivI64
                    ? dividend / divisor
                    : dividend % divisor);
          } else if (instruction.opcode == hfir::Opcode::UDivI64) {
            if (right->bits == 0)
              return false;
            bits = left->bits / right->bits;
          } else if (instruction.opcode == hfir::Opcode::AndI64) {
            bits = left->bits & right->bits;
          } else if (instruction.opcode == hfir::Opcode::OrI64) {
            bits = left->bits | right->bits;
          } else if (instruction.opcode == hfir::Opcode::XorI64) {
            bits = left->bits ^ right->bits;
          } else {
            if (right->bits >= 64)
              return false;
            const unsigned shift = static_cast<unsigned>(right->bits);
            if (instruction.opcode == hfir::Opcode::ShiftLeftI64)
              bits = left->bits << shift;
            else if (instruction.opcode == hfir::Opcode::ShiftRightSignedI64)
              bits = static_cast<std::uint64_t>(
                  static_cast<std::int64_t>(left->bits) >> shift);
            else
              bits = left->bits >> shift;
          }
          if (!assign(VMValue::scalar(hfir::ValueType::I64, bits)))
            return false;
          break;
        }
        case hfir::Opcode::TruncateI64:
        case hfir::Opcode::SignExtendI64:
        case hfir::Opcode::ZeroExtendI64: {
          const VMValue *source = operand(0);
          const VMValue *widthValue = operand(1);
          if (source == nullptr || widthValue == nullptr ||
              widthValue->bits == 0 || widthValue->bits > 64)
            return false;
          const unsigned width = static_cast<unsigned>(widthValue->bits);
          const std::uint64_t mask = width == 64
              ? std::numeric_limits<std::uint64_t>::max()
              : (std::uint64_t{1} << width) - 1;
          std::uint64_t converted = source->bits & mask;
          if (instruction.opcode == hfir::Opcode::SignExtendI64 && width < 64 &&
              (converted & (std::uint64_t{1} << (width - 1))) != 0)
            converted |= ~mask;
          if (!assign(VMValue::scalar(hfir::ValueType::I64, converted)))
            return false;
          break;
        }
        case hfir::Opcode::SignedIntToF64:
        case hfir::Opcode::UnsignedIntToF64: {
          const VMValue *source = operand(0);
          if (source == nullptr)
            return false;
          const double converted = instruction.opcode == hfir::Opcode::SignedIntToF64
              ? static_cast<double>(static_cast<std::int64_t>(source->bits))
              : static_cast<double>(source->bits);
          if (!assign(VMValue::scalar(
                  hfir::ValueType::F64,
                  std::bit_cast<std::uint64_t>(converted))))
            return false;
          break;
        }
        case hfir::Opcode::F64ToSignedInt:
        case hfir::Opcode::F64ToUnsignedInt: {
          const VMValue *source = operand(0);
          if (source == nullptr)
            return false;
          const double number = std::bit_cast<double>(source->bits);
          if (!std::isfinite(number))
            return false;
          std::uint64_t converted = 0;
          if (instruction.opcode == hfir::Opcode::F64ToSignedInt) {
            if (number < -std::ldexp(1.0, 63) ||
                number >= std::ldexp(1.0, 63))
              return false;
            converted = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(number));
          } else {
            if (number < 0.0 || number >= std::ldexp(1.0, 64))
              return false;
            converted = static_cast<std::uint64_t>(number);
          }
          if (!assign(VMValue::scalar(hfir::ValueType::I64, converted)))
            return false;
          break;
        }
        case hfir::Opcode::PackRect: {
          std::vector<std::uint8_t> bytes;
          bytes.reserve(4 * sizeof(double));
          for (std::size_t index = 0; index < 4; ++index) {
            const VMValue *component = operand(index);
            if (component == nullptr || component->type != hfir::ValueType::F64)
              return false;
            for (unsigned shift = 0; shift < 64; shift += 8)
              bytes.push_back(static_cast<std::uint8_t>(component->bits >> shift));
          }
          if (!assign(VMValue::aggregate(hfir::ValueType::Rect, bytes)))
            return false;
          break;
        }
        case hfir::Opcode::StringConcat: {
          const VMValue *left = operand(0);
          const VMValue *right = operand(1);
          if (left == nullptr || right == nullptr || left->object == nullptr ||
              right->object == nullptr)
            return false;
          void *string = IRHFObjCCreateConcatenatedString(left->object,
                                                          right->object);
          VMValue concatenated;
          if (string == nullptr || !VMValue::adoptRetainedObject(
                                       hfir::ValueType::String, string,
                                       HFHandleKindObject, concatenated) ||
              !assign(std::move(concatenated)))
            return false;
          break;
        }
        case hfir::Opcode::AddF64:
        case hfir::Opcode::SubF64:
        case hfir::Opcode::MulF64:
        case hfir::Opcode::DivF64: {
          const VMValue *left = operand(0);
          const VMValue *right = operand(1);
          if (left == nullptr || right == nullptr)
            return false;
          const double lhs = std::bit_cast<double>(left->bits);
          const double rhs = std::bit_cast<double>(right->bits);
          double value = 0;
          if (instruction.opcode == hfir::Opcode::AddF64) value = lhs + rhs;
          else if (instruction.opcode == hfir::Opcode::SubF64) value = lhs - rhs;
          else if (instruction.opcode == hfir::Opcode::MulF64) value = lhs * rhs;
          else {
            if (rhs == 0) return false;
            value = lhs / rhs;
          }
          if (!assign(VMValue::scalar(hfir::ValueType::F64,
                                      std::bit_cast<std::uint64_t>(value))))
            return false;
          break;
        }
        case hfir::Opcode::CompareEqual:
        case hfir::Opcode::CompareNotEqual:
        case hfir::Opcode::CompareLessThan:
        case hfir::Opcode::CompareLessEqual:
        case hfir::Opcode::CompareGreaterThan:
        case hfir::Opcode::CompareGreaterEqual: {
          const VMValue *left = operand(0);
          const VMValue *right = operand(1);
          if (left == nullptr || right == nullptr)
            return false;
          bool comparison = false;
          if (left->type == hfir::ValueType::Handle) {
            if (instruction.opcode != hfir::Opcode::CompareEqual &&
                instruction.opcode != hfir::Opcode::CompareNotEqual)
              return false;
            comparison = left->object == right->object;
          } else if (left->type == hfir::ValueType::F64) {
            const double lhs = std::bit_cast<double>(left->bits);
            const double rhs = std::bit_cast<double>(right->bits);
            switch (instruction.opcode) {
            case hfir::Opcode::CompareEqual: comparison = lhs == rhs; break;
            case hfir::Opcode::CompareNotEqual: comparison = lhs != rhs; break;
            case hfir::Opcode::CompareLessThan: comparison = lhs < rhs; break;
            case hfir::Opcode::CompareLessEqual: comparison = lhs <= rhs; break;
            case hfir::Opcode::CompareGreaterThan: comparison = lhs > rhs; break;
            case hfir::Opcode::CompareGreaterEqual: comparison = lhs >= rhs; break;
            default: break;
            }
          } else {
            const auto lhs = static_cast<std::int64_t>(left->bits);
            const auto rhs = static_cast<std::int64_t>(right->bits);
            switch (instruction.opcode) {
            case hfir::Opcode::CompareEqual: comparison = lhs == rhs; break;
            case hfir::Opcode::CompareNotEqual: comparison = lhs != rhs; break;
            case hfir::Opcode::CompareLessThan: comparison = lhs < rhs; break;
            case hfir::Opcode::CompareLessEqual: comparison = lhs <= rhs; break;
            case hfir::Opcode::CompareGreaterThan: comparison = lhs > rhs; break;
            case hfir::Opcode::CompareGreaterEqual: comparison = lhs >= rhs; break;
            default: break;
            }
          }
          if (!assign(VMValue::scalar(hfir::ValueType::Bool, comparison)))
            return false;
          break;
        }
        case hfir::Opcode::LocalLoad: {
          const std::uint32_t index = instruction.operands[0].index;
          if (index >= locals.size() || !locals[index].initialized ||
              !assign(locals[index]))
            return false;
          break;
        }
        case hfir::Opcode::LocalStore: {
          const std::uint32_t index = instruction.operands[0].index;
          const VMValue *source = operand(1);
          if (index >= locals.size() || source == nullptr ||
              source->type != function.localTypes[index])
            return false;
          locals[index] = *source;
          break;
        }
        case hfir::Opcode::ObjectClass: {
          const hfir::HostImport &import =
              package_.imports[instruction.operands[0].index];
          VMValue returned;
          if (!invokeHostImport(import, HFInvalidHandle(), {}, returned) ||
              !assign(std::move(returned)))
            return false;
          break;
        }
        case hfir::Opcode::ObjectConstruct:
        case hfir::Opcode::ObjectInvoke:
        case hfir::Opcode::HostCall: {
          const hfir::HostImport &import =
              package_.imports[instruction.operands[0].index];
          std::size_t cursor = 1;
          HFHandle receiver = HFInvalidHandle();
          if (import.hasReceiver) {
            const VMValue *receiverValue = operand(cursor++);
            if (receiverValue == nullptr ||
                receiverValue->type != hfir::ValueType::Handle ||
                receiverValue->object == nullptr)
              return false;
            receiver = receiverValue->handle;
          }
          std::vector<HFValue> hostArguments;
          hostArguments.reserve(instruction.operands.size() - cursor);
          for (; cursor < instruction.operands.size(); ++cursor) {
            const VMValue *argument = operand(cursor);
            HFValue hostValue = {};
            if (argument == nullptr || !marshalHostValue(*argument, hostValue))
              return false;
            hostArguments.push_back(hostValue);
          }
          VMValue returned;
          if (!invokeHostImport(import, receiver, hostArguments, returned))
            return false;
          if (import.returnType == hfir::ValueType::Void)
            break;
          if (!assign(std::move(returned)))
            return false;
          break;
        }
        case hfir::Opcode::ObjectRelease: {
          const hfir::Operand &released = instruction.operands[0];
          if (released.index >= registers.size())
            return false;
          registers[released.index] = {};
          break;
        }
        case hfir::Opcode::FunctionCall: {
          const std::uint32_t callee = instruction.operands[0].index;
          std::vector<VMValue> callArguments;
          for (std::size_t index = 1; index < instruction.operands.size(); ++index) {
            const VMValue *argument = operand(index);
            if (argument == nullptr)
              return false;
            callArguments.push_back(*argument);
          }
          VMValue returned;
          if (!executeFunction(callee, callArguments, returned, budget, depth + 1))
            return false;
          if (returned.type != hfir::ValueType::Void && !assign(std::move(returned)))
            return false;
          break;
        }
        case hfir::Opcode::Branch:
          predecessor = current;
          current = instruction.operands[0].index;
          transferred = true;
          break;
        case hfir::Opcode::ConditionalBranch: {
          const VMValue *condition = operand(0);
          if (condition == nullptr || condition->type != hfir::ValueType::Bool)
            return false;
          predecessor = current;
          current = instruction.operands[condition->bits ? 1 : 2].index;
          transferred = true;
          break;
        }
        case hfir::Opcode::Switch: {
          const VMValue *condition = operand(0);
          if (condition == nullptr)
            return false;
          std::uint32_t destination = instruction.operands[1].index;
          for (std::size_t index = 2; index < instruction.operands.size();
               index += 2) {
            const VMValue *caseValue = operand(index);
            if (caseValue == nullptr)
              return false;
            if (caseValue->bits == condition->bits) {
              destination = instruction.operands[index + 1].index;
              break;
            }
          }
          predecessor = current;
          current = destination;
          transferred = true;
          break;
        }
        case hfir::Opcode::Return:
          if (function.returnType == hfir::ValueType::Void) {
            result = VMValue::scalar(hfir::ValueType::Void, 0);
            return true;
          }
          if (const VMValue *returned = operand(0)) {
            result = *returned;
            return true;
          }
          return false;
        case hfir::Opcode::Trap:
          return false;
        }
        if (transferred)
          break;
      }
      if (!transferred)
        return false;
    }
  }

  static bool marshalHostValue(const VMValue &value, HFValue &result) {
    result = HFMakeValue(HFValueKindInvalid, 0);
    switch (value.type) {
    case hfir::ValueType::Bool:
      result.kind = HFValueKindBool;
      result.bits = value.bits;
      return true;
    case hfir::ValueType::I64:
      result.kind = HFValueKindSignedInteger;
      result.bits = value.bits;
      return true;
    case hfir::ValueType::F64:
      result.kind = HFValueKindFloat64;
      result.bits = value.bits;
      return true;
    case hfir::ValueType::Handle:
    case hfir::ValueType::String: {
      result = HFValueFromHostHandle(
          value.handle, value.handle.token == 0
                            ? HFValueFlagNone
                            : HFValueFlagBorrowedHostHandle);
      return true;
    }
    case hfir::ValueType::Bytes:
    case hfir::ValueType::Point:
    case hfir::ValueType::Size:
    case hfir::ValueType::Rect:
      result.kind = HFValueKindBytes;
      result.bytes = value.bytes.empty() ? nullptr : value.bytes.data();
      result.byteCount = value.bytes.size();
      return true;
    default:
      return false;
    }
  }

  static bool unmarshalHostValue(const HFValue &value,
                                 hfir::ValueType expected, VMValue &result) {
    switch (expected) {
    case hfir::ValueType::Void:
      if (value.kind != HFValueKindVoid)
        return false;
      result = VMValue::scalar(hfir::ValueType::Void, 0);
      return true;
    case hfir::ValueType::Handle:
    case hfir::ValueType::String: {
      if (value.kind != HFValueKindHostHandle)
        return false;
      HFHandle handle = HFInvalidHandle();
      if (!HFValueGetHostHandle(&value, &handle))
        return false;
      return VMValue::hostObject(
          expected, handle,
          value.flags == HFValueFlagRetainedHostHandle, result);
    }
    case hfir::ValueType::Bool:
      if (value.kind != HFValueKindBool || value.bits > 1)
        return false;
      result = VMValue::scalar(expected, value.bits);
      return true;
    case hfir::ValueType::I64:
      if (value.kind != HFValueKindSignedInteger &&
          value.kind != HFValueKindUnsignedInteger)
        return false;
      result = VMValue::scalar(expected, value.bits);
      return true;
    case hfir::ValueType::F64:
      if (value.kind != HFValueKindFloat64)
        return false;
      result = VMValue::scalar(expected, value.bits);
      return true;
    case hfir::ValueType::Bytes:
    case hfir::ValueType::Point:
    case hfir::ValueType::Size:
    case hfir::ValueType::Rect:
      if (value.kind != HFValueKindBytes ||
          (value.byteCount != 0 && value.bytes == nullptr))
        return false;
      result = VMValue::aggregate(
          expected,
          value.byteCount == 0
              ? std::vector<std::uint8_t>{}
              : std::vector<std::uint8_t>(
                    static_cast<const std::uint8_t *>(value.bytes),
                    static_cast<const std::uint8_t *>(value.bytes) +
                        value.byteCount));
      return true;
    }
    return false;
  }

  static HFValueKind hostValueKind(hfir::ValueType type) {
    switch (type) {
    case hfir::ValueType::Void: return HFValueKindVoid;
    case hfir::ValueType::Bool: return HFValueKindBool;
    case hfir::ValueType::I64: return HFValueKindSignedInteger;
    case hfir::ValueType::F64: return HFValueKindFloat64;
    case hfir::ValueType::Handle:
    case hfir::ValueType::String: return HFValueKindHostHandle;
    case hfir::ValueType::Bytes:
    case hfir::ValueType::Point:
    case hfir::ValueType::Size:
    case hfir::ValueType::Rect: return HFValueKindBytes;
    }
    return HFValueKindInvalid;
  }

  static bool buildHostDescriptor(
      const hfir::HostImport &import,
      std::vector<HFValueKind> &argumentKinds,
      HFHostCallDescriptor &descriptor) {
    argumentKinds.clear();
    argumentKinds.reserve(import.parameterTypes.size());
    for (hfir::ValueType type : import.parameterTypes)
      argumentKinds.push_back(hostValueKind(type));

    descriptor = {};
    descriptor.abiVersion = HF_HOST_ADAPTER_ABI_VERSION;
    descriptor.structSize = sizeof(HFHostCallDescriptor);
    descriptor.importID = import.id;
    descriptor.returnKind = hostValueKind(import.returnType);
    descriptor.argumentCount = static_cast<std::uint32_t>(argumentKinds.size());
    descriptor.owner = import.owner.c_str();
    descriptor.name = import.name.c_str();
    descriptor.typeEncoding = import.typeEncoding.c_str();
    descriptor.argumentKinds =
        argumentKinds.empty() ? nullptr : argumentKinds.data();
    descriptor.flags = import.hasReceiver ? HFHostCallFlagHasReceiver
                                          : HFHostCallFlagNone;

    switch (import.kind) {
    case hfir::HostImportKind::Class:
      descriptor.language = HFHostLanguageObjectiveC;
      descriptor.callKind = HFHostCallKindClassLookup;
      descriptor.flags |= HFHostCallFlagMainThreadOnly;
      break;
    case hfir::HostImportKind::Constructor:
      descriptor.language = HFHostLanguageObjectiveC;
      descriptor.callKind = HFHostCallKindConstructor;
      descriptor.flags |= HFHostCallFlagMainThreadOnly;
      break;
    case hfir::HostImportKind::Method:
    case hfir::HostImportKind::Service:
      descriptor.language = HFHostLanguageObjectiveC;
      descriptor.callKind = import.hasReceiver ? HFHostCallKindInstanceMethod
                                               : HFHostCallKindStaticMethod;
      descriptor.flags |= HFHostCallFlagMainThreadOnly;
      break;
    case hfir::HostImportKind::NativeC:
      descriptor.language = HFHostLanguageC;
      descriptor.callKind = HFHostCallKindFunction;
      break;
    case hfir::HostImportKind::NativeSwift:
      descriptor.language = HFHostLanguageSwift;
      descriptor.callKind = import.hasReceiver ? HFHostCallKindInstanceMethod
                                               : HFHostCallKindFunction;
      break;
    case hfir::HostImportKind::NativeCXX:
      descriptor.language = HFHostLanguageCXX;
      descriptor.callKind = import.hasReceiver ? HFHostCallKindInstanceMethod
                                               : HFHostCallKindFunction;
      break;
    }
    descriptor.signatureID = hf_host_call_signature_id(&descriptor);
    return descriptor.signatureID != 0;
  }

  bool preflightHostImports() {
    std::vector<HFValueKind> argumentKinds;
    HFHostCallDescriptor descriptor = {};
    for (const hfir::HostImport &import : package_.imports) {
      if (!buildHostDescriptor(import, argumentKinds, descriptor))
        return false;
      if (import.kind != hfir::HostImportKind::NativeC &&
          import.kind != hfir::HostImportKind::NativeSwift &&
          import.kind != hfir::HostImportKind::NativeCXX) {
        if (hf_host_adapter_validate(&descriptor) != HFStatusApplied)
          return false;
        continue;
      }
      HFHostAdapterLease *lease = nullptr;
      if (hf_host_adapter_acquire(&descriptor, &lease) != HFStatusApplied ||
          lease == nullptr)
        return false;
      try {
        nativeLeases_.emplace(import.id, lease);
      } catch (...) {
        hf_host_adapter_release(lease);
        throw;
      }
    }
    return true;
  }

  bool invokeHostImport(const hfir::HostImport &import, HFHandle receiver,
                        const std::vector<HFValue> &arguments,
                        VMValue &returned) {
    std::vector<HFValueKind> argumentKinds;
    HFHostCallDescriptor descriptor = {};
    if (!buildHostDescriptor(import, argumentKinds, descriptor))
      return false;

    HFHandle hostReceiver = HFInvalidHandle();
    if (import.hasReceiver) {
      if (receiver.token == 0)
        return false;
      hostReceiver = receiver;
    }
    HFValue result = HFMakeValue(HFValueKindInvalid, 0);
    HFStatus status = HFStatusExecutionFailed;
    if (import.kind == hfir::HostImportKind::NativeC ||
        import.kind == hfir::HostImportKind::NativeSwift ||
        import.kind == hfir::HostImportKind::NativeCXX) {
      const auto lease = nativeLeases_.find(import.id);
      if (lease == nativeLeases_.end())
        return false;
      if ((hf_host_adapter_lease_flags(lease->second) &
           HFHostCallFlagNoSideEffects) == 0)
        hostEffectsStarted_ = true;
      status = hf_host_adapter_invoke_leased(
          lease->second, hostReceiver,
          arguments.empty() ? nullptr : arguments.data(),
          static_cast<std::uint32_t>(arguments.size()), &result);
    } else {
      if (import.kind != hfir::HostImportKind::Class)
        hostEffectsStarted_ = true;
      status = hf_host_adapter_invoke(
          &descriptor, hostReceiver,
          arguments.empty() ? nullptr : arguments.data(),
          static_cast<std::uint32_t>(arguments.size()), &result);
    }
    if (status != HFStatusApplied)
      return false;
    if (!unmarshalHostValue(result, import.returnType, returned)) {
      if (result.kind == HFValueKindHostHandle && result.bits != 0 &&
          result.flags == HFValueFlagRetainedHostHandle) {
        HFHandle returnedHandle = HFInvalidHandle();
        if (HFValueGetHostHandle(&result, &returnedHandle))
          hf_host_handle_release(returnedHandle);
      }
      return false;
    }
    return true;
  }

public:
  static bool encodeResult(const VMValue &source, HFValue &result) {
    return writeFrameResult(source, result);
  }

private:
  const hfir::Package &package_;
  std::unordered_map<std::uint64_t, HFHostAdapterLease *> nativeLeases_;
  bool hostEffectsStarted_ = false;
};

HFStatus finish(HFPatchFrame *frame, HFStatus status) {
  if (frame != nullptr)
    frame->status = status;
  return status;
}

} // namespace

HFStatus hf_hfir_vm_install(const void *bytes, size_t byteCount,
                            HFIRPatchHandle *handle) {
  return PatchRegistry::shared().install(bytes, byteCount, handle);
}

HFStatus hf_hfir_vm_activate(HFIRPatchHandle handle) {
  return PatchRegistry::shared().activate(handle);
}

HFStatus hf_hfir_vm_deactivate(HFIRPatchHandle handle) {
  return PatchRegistry::shared().deactivate(handle);
}

HFStatus hf_hfir_vm_uninstall(HFIRPatchHandle handle) {
  return PatchRegistry::shared().uninstall(handle);
}

HFStatus hf_hfir_vm_invoke(HFPatchFrame *frame) {
  if (frame == nullptr)
    return HFStatusInvalidFrame;
  if (frame->abiVersion != HF_ABI_VERSION)
    return finish(frame, HFStatusABIVersionMismatch);
  if (frame->structSize != sizeof(HFPatchFrame) || frame->reserved != 0)
    return finish(frame, HFStatusInvalidFrame);
  if ((frame->flags & ~HFPatchFrameFlagHasReceiver) != 0)
    return finish(frame, HFStatusInvalidFrame);
  if (frame->argumentCount > HF_MAX_SCALAR_ARGUMENT_COUNT ||
      (frame->argumentCount == 0) != (frame->arguments == nullptr))
    return finish(frame, HFStatusInvalidArguments);
  const bool hasReceiver =
      (frame->flags & HFPatchFrameFlagHasReceiver) != 0;
  if (hasReceiver) {
    if (frame->receiver.kind != HFHandleKindObject ||
        hf_host_handle_validate(frame->receiver) != HFStatusApplied)
      return finish(frame, HFStatusInvalidFrame);
  } else if (frame->receiver.token != 0 || frame->receiver.generation != 0 ||
             frame->receiver.kind != HFHandleKindInvalid ||
             frame->receiver.flags != HFHandleFlagNone) {
    return finish(frame, HFStatusInvalidFrame);
  }

  const auto patch = PatchRegistry::shared().active(frame->targetID);
  if (!patch)
    return finish(frame, HFStatusNoPatch);
  if (patch->package.target.signatureID != frame->signatureID)
    return finish(frame, HFStatusSignatureMismatch);
  try {
    Executor executor(patch->package);
    try {
      VMValue result;
      if (!executor.invoke(*frame, result))
        return finish(frame, executor.hostEffectsStarted()
                                 ? HFStatusExecutionCommitted
                                 : HFStatusExecutionFailed);
      if (!Executor::encodeResult(result, frame->result))
        return finish(frame, executor.hostEffectsStarted()
                                 ? HFStatusExecutionCommitted
                                 : HFStatusInvalidResult);
      return finish(frame, HFStatusApplied);
    } catch (...) {
      return finish(frame, executor.hostEffectsStarted()
                               ? HFStatusExecutionCommitted
                               : HFStatusExecutionFailed);
    }
  } catch (...) {
    return finish(frame, HFStatusExecutionFailed);
  }
}
