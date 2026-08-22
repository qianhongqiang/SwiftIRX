#include "HFIRRuntime.h"

#include "../Bridge/IRHotfixObjCBridge.h"
#include "../Format/HFPatchContainer.h"

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

struct VMValue {
  hfir::ValueType type = hfir::ValueType::Void;
  std::uint64_t bits = 0;
  void *object = nullptr;
  std::shared_ptr<void> lifetime;
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

  static VMValue borrowedObject(hfir::ValueType type, void *object) {
    VMValue value;
    value.type = type;
    value.object = object;
    value.initialized = true;
    return value;
  }

  static VMValue retainedObject(hfir::ValueType type, void *object) {
    VMValue value = borrowedObject(type, object);
    if (object != nullptr) {
      value.lifetime = std::shared_ptr<void>(
          object, [](void *pointer) { IRHFObjCReleaseRetainedObject(pointer); });
    }
    return value;
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

  bool invoke(const HFPatchFrame &frame, VMValue &result) {
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
      arguments.push_back(VMValue::borrowedObject(
          hfir::ValueType::Handle,
          reinterpret_cast<void *>(static_cast<std::uintptr_t>(
              frame.receiver.token))));
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
        auto operand = [&](std::size_t index) -> const VMValue * {
          if (index >= instruction.operands.size() ||
              instruction.operands[index].kind != hfir::OperandKind::Register ||
              instruction.operands[index].index >= registers.size())
            return nullptr;
          const VMValue &value = registers[instruction.operands[index].index];
          return value.initialized ? &value : nullptr;
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
            value = VMValue::retainedObject(hfir::ValueType::String, string);
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
            value = VMValue::borrowedObject(hfir::ValueType::Handle, nullptr);
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
        case hfir::Opcode::DivI64: {
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
          else {
            const auto divisor = static_cast<std::int64_t>(right->bits);
            const auto dividend = static_cast<std::int64_t>(left->bits);
            if (divisor == 0 ||
                (dividend == std::numeric_limits<std::int64_t>::min() &&
                 divisor == -1))
              return false;
            bits = static_cast<std::uint64_t>(dividend / divisor);
          }
          if (!assign(VMValue::scalar(hfir::ValueType::I64, bits)))
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
          if (!IRHFObjCIsMainThread())
            return false;
          const hfir::HostImport &import =
              package_.imports[instruction.operands[0].index];
          void *objectClass = IRHFObjCLookUpClass(import.owner.c_str());
          if (objectClass == nullptr ||
              !assign(VMValue::borrowedObject(hfir::ValueType::Handle,
                                              objectClass)))
            return false;
          break;
        }
        case hfir::Opcode::ObjectConstruct:
        case hfir::Opcode::ObjectInvoke: {
          if (!IRHFObjCIsMainThread())
            return false;
          const hfir::HostImport &import =
              package_.imports[instruction.operands[0].index];
          std::size_t cursor = 1;
          void *receiver = nullptr;
          if (import.hasReceiver) {
            const VMValue *receiverValue = operand(cursor++);
            if (receiverValue == nullptr ||
                receiverValue->type != hfir::ValueType::Handle ||
                receiverValue->object == nullptr)
              return false;
            receiver = receiverValue->object;
          }
          std::vector<IRHFValue> hostArguments;
          hostArguments.reserve(instruction.operands.size() - cursor);
          for (; cursor < instruction.operands.size(); ++cursor) {
            const VMValue *argument = operand(cursor);
            IRHFValue hostValue = {};
            if (argument == nullptr || !marshalHostValue(*argument, hostValue))
              return false;
            hostArguments.push_back(hostValue);
          }
          IRHFObjCInvocationResult invocation =
              instruction.opcode == hfir::Opcode::ObjectConstruct
                  ? IRHFObjCConstruct(receiver, import.name.c_str(),
                                      hostArguments.data(), hostArguments.size())
                  : IRHFObjCInvoke(receiver, import.name.c_str(),
                                   hostArguments.data(), hostArguments.size());
          if (invocation.status != IRHFObjCInvocationStatusSuccess)
            return false;
          if (import.returnType == hfir::ValueType::Void)
            break;
          VMValue returned;
          if (!unmarshalHostValue(invocation.value, import.returnType, returned) ||
              !assign(std::move(returned)))
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

  static bool marshalHostValue(const VMValue &value, IRHFValue &result) {
    result = {};
    switch (value.type) {
    case hfir::ValueType::Bool:
      result.kind = IRHFValueKindBool;
      result.bits = value.bits;
      return true;
    case hfir::ValueType::I64:
      result.kind = IRHFValueKindSignedInteger;
      result.bits = value.bits;
      return true;
    case hfir::ValueType::F64:
      result.kind = IRHFValueKindFloat64;
      result.bits = value.bits;
      return true;
    case hfir::ValueType::Handle:
    case hfir::ValueType::String:
      result.kind = IRHFValueKindObject;
      result.bits = static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(value.object));
      return true;
    case hfir::ValueType::Bytes:
    case hfir::ValueType::Point:
    case hfir::ValueType::Size:
    case hfir::ValueType::Rect:
      result.kind = IRHFValueKindBytes;
      result.bytes = value.bytes.data();
      result.byteCount = value.bytes.size();
      return true;
    default:
      return false;
    }
  }

  static bool unmarshalHostValue(const IRHFValue &value,
                                 hfir::ValueType expected, VMValue &result) {
    switch (expected) {
    case hfir::ValueType::Handle:
      if (value.kind != IRHFValueKindObject &&
          value.kind != IRHFValueKindPointer)
        return false;
      result = VMValue::retainedObject(
          expected, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                        value.bits)));
      return true;
    case hfir::ValueType::Bool:
      if (value.kind != IRHFValueKindBool || value.bits > 1)
        return false;
      result = VMValue::scalar(expected, value.bits);
      return true;
    case hfir::ValueType::I64:
      if (value.kind != IRHFValueKindSignedInteger &&
          value.kind != IRHFValueKindUnsignedInteger)
        return false;
      result = VMValue::scalar(expected, value.bits);
      return true;
    case hfir::ValueType::F64:
      if (value.kind != IRHFValueKindFloat64)
        return false;
      result = VMValue::scalar(expected, value.bits);
      return true;
    default:
      return false;
    }
  }

public:
  static bool encodeResult(const VMValue &source, HFValue &result) {
    return writeFrameResult(source, result);
  }

private:
  const hfir::Package &package_;
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
    if (frame->receiver.token == 0 || frame->receiver.generation != 0 ||
        frame->receiver.kind != HFHandleKindObject ||
        frame->receiver.flags !=
            (HFHandleFlagBorrowed | HFHandleFlagBorrowedAddress))
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
    VMValue result;
    if (!executor.invoke(*frame, result))
      return finish(frame, HFStatusExecutionFailed);
    if (!Executor::encodeResult(result, frame->result))
      return finish(frame, HFStatusInvalidResult);
    return finish(frame, HFStatusApplied);
  } catch (...) {
    return finish(frame, HFStatusExecutionFailed);
  }
}
