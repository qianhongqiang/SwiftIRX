#include "HFIRLowering.h"

#include "HFPatchFrame.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace llvm;

namespace irhotfix::lowering {
namespace {

struct RegisterValue {
  std::uint32_t index = 0;
  hfir::ValueType type = hfir::ValueType::Void;
};

std::uint64_t fnv1a64(StringRef text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : text.bytes()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string importedSwiftClassName(StringRef symbol) {
  const std::size_t marker = symbol.find("$sSo");
  if (marker == StringRef::npos)
    return {};
  std::size_t cursor = marker + 4;
  std::size_t length = 0;
  while (cursor < symbol.size() && std::isdigit(symbol[cursor])) {
    length = length * 10 + static_cast<unsigned>(symbol[cursor] - '0');
    ++cursor;
  }
  if (length == 0 || cursor + length > symbol.size())
    return {};
  return symbol.substr(cursor, length).str();
}

std::string selectorFromGlobal(const Value *value) {
  value = value == nullptr ? nullptr : value->stripPointerCasts();
  const auto *load = dyn_cast_or_null<LoadInst>(value);
  if (load != nullptr)
    value = load->getPointerOperand()->stripPointerCasts();
  const auto *global = dyn_cast_or_null<GlobalValue>(value);
  if (global == nullptr)
    return {};
  const StringRef name = global->getName();
  const std::size_t marker = name.find("L_selector(");
  if (marker == StringRef::npos)
    return {};
  const std::size_t start = marker + StringRef("L_selector(").size();
  const std::size_t end = name.find(')', start);
  return end == StringRef::npos ? std::string() : name.slice(start, end).str();
}

std::string classFromGlobal(const Value *value) {
  value = value == nullptr ? nullptr : value->stripPointerCasts();
  const auto *global = dyn_cast_or_null<GlobalValue>(value);
  if (global == nullptr)
    return {};
  constexpr StringLiteral marker = "OBJC_CLASS_REF_$_";
  const StringRef name = global->getName();
  const std::size_t position = name.find(marker);
  return position == StringRef::npos
             ? std::string()
             : name.drop_front(position + marker.size()).str();
}

std::vector<std::uint8_t> stringBytesFromSymbol(StringRef symbol) {
  const std::size_t marker = symbol.find(".str.");
  if (marker == StringRef::npos)
    return {};
  std::size_t cursor = marker + 5;
  while (cursor < symbol.size() && std::isdigit(symbol[cursor]))
    ++cursor;
  if (cursor >= symbol.size() || symbol[cursor] != '.')
    return {};
  ++cursor;
  std::vector<std::uint8_t> bytes;
  while (cursor < symbol.size()) {
    if (symbol[cursor] == '\\' && cursor + 2 < symbol.size() &&
        isHexDigit(symbol[cursor + 1]) && isHexDigit(symbol[cursor + 2])) {
      unsigned value = 0;
      if (to_integer(symbol.substr(cursor + 1, 2), value, 16)) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        cursor += 3;
        continue;
      }
    }
    bytes.push_back(static_cast<std::uint8_t>(symbol[cursor++]));
  }
  return bytes;
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

Function *directCallee(CallBase &call) {
  return dyn_cast<Function>(call.getCalledOperand()->stripPointerCasts());
}

const Function *directCallee(const CallBase &call) {
  return dyn_cast<Function>(call.getCalledOperand()->stripPointerCasts());
}

class FunctionLowerer {
public:
  FunctionLowerer(
      Function &function, hfir::Package &package, std::uint32_t functionIndex,
      const std::unordered_map<const Function *, std::uint32_t> &functionIndices)
      : function_(function), package_(package), functionIndex_(functionIndex),
        functionIndices_(functionIndices) {
    output_.name = functionIndex == 0
        ? "patch"
        : "helper." + std::to_string(functionIndex);
  }

  bool run(std::string &error) {
    error_ = &error;
    error.clear();
    if (!prepare())
      return false;
    for (BasicBlock &block : function_) {
      hfir::BasicBlock lowered;
      lowered.id = blockIDs_.at(&block);
      current_ = &lowered;
      for (Instruction &instruction : block) {
        if (!lowerInstruction(instruction))
          return false;
      }
      output_.blocks.push_back(std::move(lowered));
    }
    package_.functions[functionIndex_] = std::move(output_);
    return true;
  }

private:
  bool prepare() {
    output_.returnType = typeForLLVM(function_.getReturnType()).value_or(
        hfir::ValueType::Void);
    if (!function_.getReturnType()->isVoidTy() &&
        !typeForLLVM(function_.getReturnType()))
      return fail("unsupported function return type");

    for (Argument &argument : function_.args()) {
      const auto type = typeForLLVM(argument.getType());
      if (!type)
        return fail("unsupported function parameter type");
      output_.parameterTypes.push_back(*type);
      assignRegister(&argument, *type);
    }
    std::uint32_t blockID = 0;
    for (BasicBlock &block : function_)
      blockIDs_[&block] = blockID++;
    output_.entryBlock = blockIDs_.at(&function_.getEntryBlock());

    for (BasicBlock &block : function_) {
      for (Instruction &instruction : block) {
        if (auto *allocation = dyn_cast<AllocaInst>(&instruction)) {
          const auto type = typeForLLVM(allocation->getAllocatedType());
          if (type) {
            const std::uint32_t index =
                static_cast<std::uint32_t>(output_.localTypes.size());
            locals_[allocation] = index;
            localPointers_[allocation] = index;
            allocationLocals_[allocation].push_back(index);
            output_.localTypes.push_back(*type);
          } else if (allocation->getAllocatedType()->isAggregateType()) {
            aggregateAllocations_.insert(allocation);
          } else {
            return fail(instruction, "unsupported alloca type");
          }
          continue;
        }
        if (auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
          auto *allocation = aggregateRoot(gep);
          if (allocation != nullptr && aggregateAllocations_.contains(allocation))
            continue;
        }
        const auto type = semanticType(&instruction);
        if (type && needsRegister(instruction))
          assignRegister(&instruction, *type);
      }
    }
    for (BasicBlock &block : function_) {
      for (Instruction &instruction : block) {
        Value *memoryPointer = nullptr;
        Type *memoryType = nullptr;
        if (auto *store = dyn_cast<StoreInst>(&instruction)) {
          memoryPointer = store->getPointerOperand()->stripPointerCasts();
          memoryType = store->getValueOperand()->getType();
        } else if (auto *load = dyn_cast<LoadInst>(&instruction)) {
          memoryPointer = load->getPointerOperand()->stripPointerCasts();
          memoryType = load->getType();
        }
        if (const auto *allocation = aggregateRoot(memoryPointer);
            allocation != nullptr && aggregateAllocations_.contains(allocation)) {
          const auto fieldType = typeForLLVM(memoryType);
          if (!fieldType || *fieldType == hfir::ValueType::Void)
            return fail(instruction, "unsupported aggregate root access");
          std::string key = allocation->getName().str();
          std::vector<const GetElementPtrInst *> memoryChain;
          const Value *memoryCursor = memoryPointer;
          while (const auto *element =
                     dyn_cast_or_null<GetElementPtrInst>(memoryCursor)) {
            memoryChain.push_back(element);
            memoryCursor = element->getPointerOperand()->stripPointerCasts();
          }
          if (memoryChain.empty()) {
            key += ".0.0";
          } else {
            for (auto element = memoryChain.rbegin();
                 element != memoryChain.rend(); ++element) {
              for (Value *index : (*element)->indices()) {
                const auto *constant = dyn_cast<ConstantInt>(index);
                if (constant == nullptr)
                  return fail(instruction,
                              "dynamic aggregate field index is unsupported");
                key += "." + std::to_string(constant->getSExtValue());
              }
            }
          }
          std::uint32_t local = 0;
          if (const auto found = aggregateFieldLocals_.find(key);
              found != aggregateFieldLocals_.end()) {
            local = found->second;
          } else {
            local = static_cast<std::uint32_t>(output_.localTypes.size());
            output_.localTypes.push_back(*fieldType);
            aggregateFieldLocals_[key] = local;
            allocationLocals_[allocation].push_back(local);
          }
          localPointers_[memoryPointer] = local;
        }
        auto *gep = dyn_cast<GetElementPtrInst>(&instruction);
        if (gep == nullptr)
          continue;
        auto *allocation = aggregateRoot(gep);
        if (allocation == nullptr || !aggregateAllocations_.contains(allocation))
          continue;
        const auto elementType = typeForLLVM(gep->getResultElementType());
        std::string key = allocation->getName().str();
        std::vector<const GetElementPtrInst *> chain;
        const Value *cursor = gep;
        while (const auto *element = dyn_cast<GetElementPtrInst>(cursor)) {
          chain.push_back(element);
          cursor = element->getPointerOperand()->stripPointerCasts();
        }
        for (auto element = chain.rbegin(); element != chain.rend(); ++element) {
          for (Value *index : (*element)->indices()) {
            const auto *constant = dyn_cast<ConstantInt>(index);
            if (constant == nullptr)
              return fail(instruction,
                          "dynamic aggregate field index is unsupported");
            key += "." + std::to_string(constant->getSExtValue());
          }
        }
        if (!elementType || *elementType == hfir::ValueType::Void) {
          if (gep->getResultElementType()->isAggregateType())
            continue;
          return fail(instruction, "unsupported aggregate field type");
        }
        std::uint32_t local = 0;
        if (const auto found = aggregateFieldLocals_.find(key);
            found != aggregateFieldLocals_.end()) {
          local = found->second;
        } else {
          local = static_cast<std::uint32_t>(output_.localTypes.size());
          output_.localTypes.push_back(*elementType);
          aggregateFieldLocals_[key] = local;
          allocationLocals_[allocation].push_back(local);
        }
        localPointers_[gep] = local;
      }
    }
    return true;
  }

  const AllocaInst *aggregateRoot(const Value *value) const {
    value = value == nullptr ? nullptr : value->stripPointerCasts();
    while (const auto *gep = dyn_cast_or_null<GetElementPtrInst>(value))
      value = gep->getPointerOperand()->stripPointerCasts();
    return dyn_cast_or_null<AllocaInst>(value);
  }

  std::optional<hfir::ValueType> typeForLLVM(Type *type) const {
    if (type->isVoidTy()) return hfir::ValueType::Void;
    if (type->isIntegerTy(1)) return hfir::ValueType::Bool;
    if (type->isIntegerTy()) return hfir::ValueType::I64;
    if (type->isFloatTy() || type->isDoubleTy()) return hfir::ValueType::F64;
    if (type->isPointerTy()) return hfir::ValueType::Handle;
    return std::nullopt;
  }

  std::optional<hfir::ValueType> semanticType(Value *value) {
    if (const auto found = semanticTypes_.find(value);
        found != semanticTypes_.end())
      return found->second;
    if (!semanticVisiting_.emplace(value, true).second)
      return typeForLLVM(value->getType());

    std::optional<hfir::ValueType> type;
    if (auto *call = dyn_cast<CallBase>(value)) {
      const Function *callee = directCallee(*call);
      const StringRef name = callee == nullptr ? StringRef() : callee->getName();
      if (name.contains("_bridgeToObjectiveC"))
        type = hfir::ValueType::String;
      else if (name.contains("SS1poi") || name.contains("StringV1poi"))
        type = hfir::ValueType::String;
      else if (name.contains("objc.retain") ||
               name.contains("retainAutoreleasedReturnValue") ||
               name == "objc_opt_self")
        type = semanticType(call->getArgOperand(0));
      else
        type = typeForLLVM(value->getType());
    } else if (auto *extract = dyn_cast<ExtractValueInst>(value)) {
      if (auto *call = dyn_cast<CallBase>(extract->getAggregateOperand())) {
        const Function *callee = directCallee(*call);
        const StringRef name = callee == nullptr ? StringRef() : callee->getName();
        if (name.contains("_builtinStringLiteral"))
          type = hfir::ValueType::String;
        else if (name.contains("SS1poi") || name.contains("StringV1poi"))
          type = hfir::ValueType::String;
        else if (name.ends_with("CMa"))
          type = hfir::ValueType::Handle;
      }
      if (!type)
        type = typeForLLVM(value->getType());
    } else if (auto *cast = dyn_cast<CastInst>(value)) {
      if (cast->getType()->isPointerTy() ||
          cast->getOperand(0)->getType()->isPointerTy())
        type = semanticType(cast->getOperand(0));
      else
        type = typeForLLVM(cast->getType());
    } else if (auto *phi = dyn_cast<PHINode>(value)) {
      for (Value *incoming : phi->incoming_values()) {
        const auto incomingType = semanticType(incoming);
        if (incomingType == hfir::ValueType::Handle ||
            incomingType == hfir::ValueType::String) {
          type = incomingType;
          break;
        }
      }
      if (!type)
        type = typeForLLVM(value->getType());
    } else if (auto *load = dyn_cast<LoadInst>(value)) {
      if (const auto *allocation = dyn_cast<AllocaInst>(
              load->getPointerOperand()->stripPointerCasts())) {
        if (const auto found = locals_.find(allocation); found != locals_.end())
          type = output_.localTypes[found->second];
        else
          type = typeForLLVM(value->getType());
      } else if (!selectorFromGlobal(load).empty())
        type = std::nullopt;
      else
        type = typeForLLVM(value->getType());
    } else {
      type = typeForLLVM(value->getType());
    }
    semanticVisiting_.erase(value);
    semanticTypes_[value] = type;
    return type;
  }

  bool needsRegister(Instruction &instruction) {
    if (instruction.getType()->isVoidTy() || isa<AllocaInst>(instruction) ||
        instruction.isTerminator())
      return false;
    if (localPointers_.contains(&instruction))
      return false;
    if (auto *load = dyn_cast<LoadInst>(&instruction))
      return selectorFromGlobal(load).empty();
    if (auto *call = dyn_cast<CallBase>(&instruction)) {
      if (call->isInlineAsm()) return false;
      const Function *callee = directCallee(*call);
      const StringRef name = callee == nullptr ? StringRef() : callee->getName();
      if (name.ends_with("CMa") || name.contains("_builtinStringLiteral"))
        return false;
    }
    return semanticType(&instruction).has_value();
  }

  RegisterValue assignRegister(Value *value, hfir::ValueType type) {
    RegisterValue assigned{static_cast<std::uint32_t>(output_.registerTypes.size()),
                           type};
    registers_[value] = assigned;
    output_.registerTypes.push_back(type);
    return assigned;
  }

  std::optional<RegisterValue> registerFor(Value *value,
                                            hfir::ValueType expected) {
    if (const auto found = registers_.find(value); found != registers_.end())
      return found->second.type == expected ? std::optional(found->second)
                                            : std::nullopt;
    if (auto *constant = dyn_cast<ConstantInt>(value)) {
      if (expected == hfir::ValueType::Handle && constant->isZero())
        return emitConstant({hfir::ConstantKind::NullHandle, 0, {}}, expected);
      if (expected == hfir::ValueType::Bool && constant->getValue().ule(1))
        return emitConstant({hfir::ConstantKind::Bool,
                             constant->getZExtValue(), {}}, expected);
      if (expected == hfir::ValueType::I64)
        return emitConstant({hfir::ConstantKind::I64,
                             constant->getZExtValue(), {}}, expected);
    }
    if (auto *constant = dyn_cast<ConstantFP>(value)) {
      if (expected == hfir::ValueType::F64) {
        const double number = constant->getValueAPF().convertToDouble();
        return emitConstant({hfir::ConstantKind::F64,
                             std::bit_cast<std::uint64_t>(number), {}}, expected);
      }
    }
    if (isa<ConstantPointerNull>(value) && expected == hfir::ValueType::Handle)
      return emitConstant({hfir::ConstantKind::NullHandle, 0, {}}, expected);
    return std::nullopt;
  }

  std::optional<hfir::Operand> constantOperand(Value *value,
                                                hfir::ValueType expected) {
    hfir::Constant constant;
    if (auto *integer = dyn_cast<ConstantInt>(value)) {
      if (expected == hfir::ValueType::Handle && integer->isZero())
        constant = {hfir::ConstantKind::NullHandle, 0, {}};
      else if (expected == hfir::ValueType::Bool &&
               integer->getValue().ule(1))
        constant = {hfir::ConstantKind::Bool, integer->getZExtValue(), {}};
      else if (expected == hfir::ValueType::I64)
        constant = {hfir::ConstantKind::I64, integer->getZExtValue(), {}};
      else
        return std::nullopt;
    } else if (auto *floating = dyn_cast<ConstantFP>(value)) {
      if (expected != hfir::ValueType::F64)
        return std::nullopt;
      const double number = floating->getValueAPF().convertToDouble();
      constant = {hfir::ConstantKind::F64,
                  std::bit_cast<std::uint64_t>(number), {}};
    } else if (isa<ConstantPointerNull>(value) &&
               expected == hfir::ValueType::Handle) {
      constant = {hfir::ConstantKind::NullHandle, 0, {}};
    } else {
      return std::nullopt;
    }
    const std::uint32_t index =
        static_cast<std::uint32_t>(package_.constants.size());
    package_.constants.push_back(std::move(constant));
    return hfir::Operand{hfir::OperandKind::Constant, expected, index};
  }

  RegisterValue emitConstant(hfir::Constant constant, hfir::ValueType type,
                             hfir::Opcode opcode = hfir::Opcode::Constant) {
    const std::uint32_t constantIndex =
        static_cast<std::uint32_t>(package_.constants.size());
    package_.constants.push_back(std::move(constant));
    RegisterValue result = assignTemporary(type);
    current_->instructions.push_back(
        {opcode, result.index, type,
         {{hfir::OperandKind::Constant, type, constantIndex}}});
    return result;
  }

  RegisterValue assignTemporary(hfir::ValueType type) {
    RegisterValue assigned{static_cast<std::uint32_t>(output_.registerTypes.size()),
                           type};
    output_.registerTypes.push_back(type);
    return assigned;
  }

  hfir::Operand reg(RegisterValue value) const {
    return {hfir::OperandKind::Register, value.type, value.index};
  }

  hfir::Operand block(const BasicBlock *value) const {
    return {hfir::OperandKind::Block, hfir::ValueType::Void,
            blockIDs_.at(value)};
  }

  hfir::Operand imported(std::uint32_t index) const {
    return {hfir::OperandKind::Import, hfir::ValueType::Void, index};
  }

  hfir::Operand functionOperand(std::uint32_t index) const {
    return {hfir::OperandKind::Function, hfir::ValueType::Void, index};
  }

  bool emitResult(Instruction &source, hfir::Opcode opcode,
                  std::vector<hfir::Operand> operands) {
    const auto destination = registers_.find(&source);
    if (destination == registers_.end())
      return fail(source, "instruction has no HFIR result register");
    current_->instructions.push_back({opcode, destination->second.index,
                                      destination->second.type,
                                      std::move(operands)});
    return true;
  }

  void emitEffect(hfir::Opcode opcode, std::vector<hfir::Operand> operands) {
    current_->instructions.push_back(
        {opcode, hfir::kNoRegister, hfir::ValueType::Void,
         std::move(operands)});
  }

  std::uint32_t classImport(StringRef className) {
    return hostImport(hfir::HostImportKind::Class, className, "class",
                      hfir::ValueType::Handle, {}, false);
  }

  std::uint32_t hostImport(hfir::HostImportKind kind, StringRef owner,
                           StringRef name, hfir::ValueType returnType,
                           const std::vector<hfir::ValueType> &parameters,
                           bool hasReceiver,
                           std::optional<std::uint64_t> explicitID = std::nullopt) {
    std::string key = std::to_string(static_cast<unsigned>(kind)) + ":" +
                      owner.str() + ":" + name.str() + ":" +
                      hfir::valueTypeName(returnType) + ":" +
                      (hasReceiver ? "1" : "0");
    for (hfir::ValueType parameter : parameters)
      key += ":" + std::string(hfir::valueTypeName(parameter));
    if (const auto found = importIndices_.find(key);
        found != importIndices_.end())
      return found->second;
    hfir::HostImport import;
    import.id = explicitID.value_or(fnv1a64(key));
    import.kind = kind;
    import.owner = owner.str();
    import.name = name.str();
    import.returnType = returnType;
    import.parameterTypes = parameters;
    import.hasReceiver = hasReceiver;
    const std::uint32_t index =
        static_cast<std::uint32_t>(package_.imports.size());
    package_.imports.push_back(std::move(import));
    importIndices_[key] = index;
    return index;
  }

  bool lowerInstruction(Instruction &instruction) {
    if (auto *allocation = dyn_cast<AllocaInst>(&instruction)) {
      for (std::uint32_t local : allocationLocals_[allocation]) {
        emitEffect(hfir::Opcode::LocalAllocate,
                   {{hfir::OperandKind::Local, output_.localTypes[local],
                     local}});
      }
      return true;
    }
    if (auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
      const auto *allocation = aggregateRoot(gep);
      if (localPointers_.contains(&instruction) ||
          (allocation != nullptr &&
           aggregateAllocations_.contains(allocation)))
        return true;
    }
    if (auto *store = dyn_cast<StoreInst>(&instruction)) {
      Value *pointer = store->getPointerOperand()->stripPointerCasts();
      const auto found = localPointers_.find(pointer);
      if (found == localPointers_.end())
        return fail(instruction, "only function-local stores are supported");
      const hfir::ValueType type = output_.localTypes[found->second];
      const auto value = registerFor(store->getValueOperand(), type);
      if (!value)
        return fail(instruction, "cannot lower stored value");
      emitEffect(hfir::Opcode::LocalStore,
                 {{hfir::OperandKind::Local, type, found->second},
                  reg(*value)});
      return true;
    }
    if (auto *load = dyn_cast<LoadInst>(&instruction))
      return lowerLoad(*load);
    if (auto *binary = dyn_cast<BinaryOperator>(&instruction))
      return lowerBinary(*binary);
    if (auto *comparison = dyn_cast<ICmpInst>(&instruction))
      return lowerComparison(*comparison);
    if (auto *comparison = dyn_cast<FCmpInst>(&instruction))
      return lowerFloatComparison(*comparison);
    if (auto *cast = dyn_cast<CastInst>(&instruction))
      return lowerCast(*cast);
    if (auto *select = dyn_cast<SelectInst>(&instruction))
      return lowerSelect(*select);
    if (auto *extract = dyn_cast<ExtractValueInst>(&instruction))
      return lowerExtract(*extract);
    if (auto *phi = dyn_cast<PHINode>(&instruction)) {
      const RegisterValue destination = registers_.at(phi);
      std::vector<hfir::Operand> operands;
      for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
        Value *incomingValue = phi->getIncomingValue(index);
        if (isa<Constant>(incomingValue)) {
          const auto incoming = constantOperand(incomingValue, destination.type);
          if (!incoming)
            return fail(instruction, "cannot lower phi constant");
          operands.push_back(*incoming);
        } else {
          const auto incoming = registerFor(incomingValue, destination.type);
          if (!incoming)
            return fail(instruction, "cannot lower phi incoming value");
          operands.push_back(reg(*incoming));
        }
        operands.push_back(block(phi->getIncomingBlock(index)));
      }
      return emitResult(*phi, hfir::Opcode::Phi, std::move(operands));
    }
    if (auto *call = dyn_cast<CallBase>(&instruction))
      return lowerCall(*call);
    if (auto *branch = dyn_cast<BranchInst>(&instruction)) {
      if (branch->isUnconditional()) {
        emitEffect(hfir::Opcode::Branch, {block(branch->getSuccessor(0))});
      } else {
        const auto condition =
            registerFor(branch->getCondition(), hfir::ValueType::Bool);
        if (!condition)
          return fail(instruction, "cannot lower branch condition");
        emitEffect(hfir::Opcode::ConditionalBranch,
                   {reg(*condition), block(branch->getSuccessor(0)),
                    block(branch->getSuccessor(1))});
      }
      return true;
    }
    if (auto *switchInstruction = dyn_cast<SwitchInst>(&instruction)) {
      const hfir::ValueType type = switchInstruction->getCondition()
                                             ->getType()->isIntegerTy(1)
                                         ? hfir::ValueType::Bool
                                         : hfir::ValueType::I64;
      const auto condition = registerFor(switchInstruction->getCondition(), type);
      if (!condition)
        return fail(instruction, "cannot lower switch condition");
      std::vector<hfir::Operand> operands = {
          reg(*condition), block(switchInstruction->getDefaultDest())};
      for (const auto &entry : switchInstruction->cases()) {
        const auto caseValue = constantOperand(entry.getCaseValue(), type);
        if (!caseValue)
          return fail(instruction, "cannot lower switch case");
        operands.push_back(*caseValue);
        operands.push_back(block(entry.getCaseSuccessor()));
      }
      emitEffect(hfir::Opcode::Switch, std::move(operands));
      return true;
    }
    if (auto *returned = dyn_cast<ReturnInst>(&instruction)) {
      if (returned->getReturnValue() == nullptr) {
        emitEffect(hfir::Opcode::Return, {});
        return true;
      }
      const auto value =
          registerFor(returned->getReturnValue(), output_.returnType);
      if (!value)
        return fail(instruction, "cannot lower return value");
      emitEffect(hfir::Opcode::Return, {reg(*value)});
      return true;
    }
    if (isa<UnreachableInst>(instruction)) {
      emitEffect(hfir::Opcode::Trap, {});
      return true;
    }
    return fail(instruction, "unsupported LLVM instruction");
  }

  bool lowerLoad(LoadInst &load) {
    if (!selectorFromGlobal(&load).empty())
      return true;
    Value *pointer = load.getPointerOperand()->stripPointerCasts();
    if (const auto found = localPointers_.find(pointer);
        found != localPointers_.end()) {
      return emitResult(
          load, hfir::Opcode::LocalLoad,
          {{hfir::OperandKind::Local, output_.localTypes[found->second],
            found->second}});
    }
    const std::string className = classFromGlobal(load.getPointerOperand());
    if (!className.empty()) {
      classOrigins_[registers_.at(&load).index] = className;
      return emitResult(load, hfir::Opcode::ObjectClass,
                        {imported(classImport(className))});
    }
    return fail(load, "unsupported global or indirect load");
  }

  bool lowerBinary(BinaryOperator &binary) {
    hfir::Opcode opcode;
    switch (binary.getOpcode()) {
    case Instruction::Add: opcode = hfir::Opcode::AddI64; break;
    case Instruction::Sub: opcode = hfir::Opcode::SubI64; break;
    case Instruction::Mul: opcode = hfir::Opcode::MulI64; break;
    case Instruction::SDiv: opcode = hfir::Opcode::DivI64; break;
    case Instruction::UDiv: opcode = hfir::Opcode::UDivI64; break;
    case Instruction::SRem:
    case Instruction::URem: opcode = hfir::Opcode::RemI64; break;
    case Instruction::And: opcode = hfir::Opcode::AndI64; break;
    case Instruction::Or: opcode = hfir::Opcode::OrI64; break;
    case Instruction::Xor: opcode = hfir::Opcode::XorI64; break;
    case Instruction::Shl: opcode = hfir::Opcode::ShiftLeftI64; break;
    case Instruction::AShr: opcode = hfir::Opcode::ShiftRightSignedI64; break;
    case Instruction::LShr: opcode = hfir::Opcode::ShiftRightUnsignedI64; break;
    case Instruction::FAdd: opcode = hfir::Opcode::AddF64; break;
    case Instruction::FSub: opcode = hfir::Opcode::SubF64; break;
    case Instruction::FMul: opcode = hfir::Opcode::MulF64; break;
    case Instruction::FDiv: opcode = hfir::Opcode::DivF64; break;
    default: return fail(binary, "unsupported binary opcode");
    }
    const RegisterValue destination = registers_.at(&binary);
    const auto left = registerFor(binary.getOperand(0), destination.type);
    const auto right = registerFor(binary.getOperand(1), destination.type);
    return left && right
               ? emitResult(binary, opcode, {reg(*left), reg(*right)})
               : fail(binary, "cannot lower binary operands");
  }

  bool lowerComparison(ICmpInst &comparison) {
    hfir::Opcode opcode;
    switch (comparison.getPredicate()) {
    case CmpInst::ICMP_EQ: opcode = hfir::Opcode::CompareEqual; break;
    case CmpInst::ICMP_NE: opcode = hfir::Opcode::CompareNotEqual; break;
    case CmpInst::ICMP_SLT:
    case CmpInst::ICMP_ULT: opcode = hfir::Opcode::CompareLessThan; break;
    case CmpInst::ICMP_SLE:
    case CmpInst::ICMP_ULE: opcode = hfir::Opcode::CompareLessEqual; break;
    case CmpInst::ICMP_SGT:
    case CmpInst::ICMP_UGT: opcode = hfir::Opcode::CompareGreaterThan; break;
    case CmpInst::ICMP_SGE:
    case CmpInst::ICMP_UGE: opcode = hfir::Opcode::CompareGreaterEqual; break;
    default: return fail(comparison, "unsupported comparison predicate");
    }
    hfir::ValueType operandType =
        semanticType(comparison.getOperand(0)).value_or(hfir::ValueType::I64);
    if (operandType == hfir::ValueType::Bool &&
        semanticType(comparison.getOperand(1)) == hfir::ValueType::I64)
      operandType = hfir::ValueType::I64;
    const auto left = registerFor(comparison.getOperand(0), operandType);
    const auto right = registerFor(comparison.getOperand(1), operandType);
    return left && right
               ? emitResult(comparison, opcode, {reg(*left), reg(*right)})
               : fail(comparison, "cannot lower comparison operands");
  }

  bool lowerFloatComparison(FCmpInst &comparison) {
    hfir::Opcode opcode;
    switch (comparison.getPredicate()) {
    case CmpInst::FCMP_OEQ:
    case CmpInst::FCMP_UEQ: opcode = hfir::Opcode::CompareEqual; break;
    case CmpInst::FCMP_ONE:
    case CmpInst::FCMP_UNE: opcode = hfir::Opcode::CompareNotEqual; break;
    case CmpInst::FCMP_OLT:
    case CmpInst::FCMP_ULT: opcode = hfir::Opcode::CompareLessThan; break;
    case CmpInst::FCMP_OLE:
    case CmpInst::FCMP_ULE: opcode = hfir::Opcode::CompareLessEqual; break;
    case CmpInst::FCMP_OGT:
    case CmpInst::FCMP_UGT: opcode = hfir::Opcode::CompareGreaterThan; break;
    case CmpInst::FCMP_OGE:
    case CmpInst::FCMP_UGE: opcode = hfir::Opcode::CompareGreaterEqual; break;
    default: return fail(comparison, "unsupported floating comparison predicate");
    }
    const auto left = registerFor(comparison.getOperand(0), hfir::ValueType::F64);
    const auto right = registerFor(comparison.getOperand(1), hfir::ValueType::F64);
    return left && right
               ? emitResult(comparison, opcode, {reg(*left), reg(*right)})
               : fail(comparison, "cannot lower floating comparison operands");
  }

  bool lowerCast(CastInst &cast) {
    const RegisterValue destination = registers_.at(&cast);
    hfir::Opcode opcode = hfir::Opcode::Move;
    hfir::ValueType sourceType = destination.type;
    std::vector<hfir::Operand> operands;
    switch (cast.getOpcode()) {
    case Instruction::Trunc:
      if (cast.getDestTy()->isIntegerTy(1)) {
        const auto source = registerFor(cast.getOperand(0), hfir::ValueType::I64);
        const auto zero = registerFor(
            ConstantInt::get(cast.getOperand(0)->getType(), 0),
            hfir::ValueType::I64);
        return source && zero
                   ? emitResult(cast, hfir::Opcode::CompareNotEqual,
                                {reg(*source), reg(*zero)})
                   : fail(cast, "cannot lower integer-to-bool truncation");
      }
      opcode = hfir::Opcode::TruncateI64;
      sourceType = hfir::ValueType::I64;
      break;
    case Instruction::SExt:
      opcode = hfir::Opcode::SignExtendI64;
      sourceType = cast.getOperand(0)->getType()->isIntegerTy(1)
                       ? hfir::ValueType::Bool
                       : hfir::ValueType::I64;
      break;
    case Instruction::ZExt:
      opcode = hfir::Opcode::ZeroExtendI64;
      sourceType = cast.getOperand(0)->getType()->isIntegerTy(1)
                       ? hfir::ValueType::Bool
                       : hfir::ValueType::I64;
      break;
    case Instruction::SIToFP:
      opcode = hfir::Opcode::SignedIntToF64;
      sourceType = hfir::ValueType::I64;
      break;
    case Instruction::UIToFP:
      opcode = hfir::Opcode::UnsignedIntToF64;
      sourceType = hfir::ValueType::I64;
      break;
    case Instruction::FPToSI:
      opcode = hfir::Opcode::F64ToSignedInt;
      sourceType = hfir::ValueType::F64;
      break;
    case Instruction::FPToUI:
      opcode = hfir::Opcode::F64ToUnsignedInt;
      sourceType = hfir::ValueType::F64;
      break;
    case Instruction::FPExt:
    case Instruction::FPTrunc:
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
      break;
    default:
      return fail(cast, "unsupported cast opcode");
    }
    auto source = registerFor(cast.getOperand(0), sourceType);
    if (!source)
      return fail(cast, "cannot lower cast source");
    operands.push_back(reg(*source));
    if (opcode == hfir::Opcode::TruncateI64 ||
        opcode == hfir::Opcode::SignExtendI64 ||
        opcode == hfir::Opcode::ZeroExtendI64) {
      const unsigned width = opcode == hfir::Opcode::TruncateI64
          ? cast.getDestTy()->getIntegerBitWidth()
          : cast.getSrcTy()->getIntegerBitWidth();
      const auto widthOperand = constantOperand(
          ConstantInt::get(Type::getInt64Ty(function_.getContext()), width),
          hfir::ValueType::I64);
      operands.push_back(*widthOperand);
    }
    return emitResult(cast, opcode, std::move(operands));
  }

  bool lowerSelect(SelectInst &select) {
    const RegisterValue destination = registers_.at(&select);
    const auto condition = registerFor(select.getCondition(), hfir::ValueType::Bool);
    const auto trueValue = registerFor(select.getTrueValue(), destination.type);
    const auto falseValue = registerFor(select.getFalseValue(), destination.type);
    return condition && trueValue && falseValue
               ? emitResult(select, hfir::Opcode::Select,
                            {reg(*condition), reg(*trueValue), reg(*falseValue)})
               : fail(select, "cannot lower select operands");
  }

  bool lowerExtract(ExtractValueInst &extract) {
    auto *call = dyn_cast<CallBase>(extract.getAggregateOperand());
    const Function *callee = call == nullptr ? nullptr : directCallee(*call);
    const StringRef name = callee == nullptr ? StringRef() : callee->getName();
    if ((name.contains("SS1poi") || name.contains("StringV1poi")) &&
        registers_.contains(call)) {
      return emitResult(extract, hfir::Opcode::Move,
                        {reg(registers_.at(call))});
    }
    if (name.ends_with("CMa")) {
      const std::string className = importedSwiftClassName(name);
      if (className.empty())
        return fail(extract, "cannot decode Swift Objective-C class metadata");
      classOrigins_[registers_.at(&extract).index] = className;
      return emitResult(extract, hfir::Opcode::ObjectClass,
                        {imported(classImport(className))});
    }
    if (name.contains("_builtinStringLiteral")) {
      if (const auto found = stringLiteralRegisters_.find(call);
          found != stringLiteralRegisters_.end()) {
        return emitResult(extract, hfir::Opcode::Move,
                          {reg(found->second)});
      }
      Value *literal = call->getArgOperand(0)->stripPointerCasts();
      const auto *global = dyn_cast<GlobalValue>(literal);
      if (global == nullptr)
        return fail(extract, "string literal has no global symbol");
      const std::vector<std::uint8_t> bytes =
          stringBytesFromSymbol(global->getName());
      const std::uint32_t constantIndex =
          static_cast<std::uint32_t>(package_.constants.size());
      package_.constants.push_back(
          {hfir::ConstantKind::String, 0, bytes});
      const bool emitted = emitResult(
          extract, hfir::Opcode::StringConstant,
          {{hfir::OperandKind::Constant, hfir::ValueType::String,
            constantIndex}});
      if (emitted)
        stringLiteralRegisters_[call] = registers_.at(&extract);
      return emitted;
    }
    return fail(extract, "unsupported aggregate extraction");
  }

  bool lowerCall(CallBase &call) {
    if (call.isInlineAsm())
      return true;
    Function *callee = directCallee(call);
    if (callee == nullptr)
      return fail(call, "indirect calls are unsupported");
    const StringRef name = callee->getName();
    if (name.starts_with("llvm.memset") || name.starts_with("llvm.lifetime") ||
        name == "swift_bridgeObjectRelease")
      return true;
    if (name.contains("objc.release"))
      return true;
    if (name.contains("objc.retain") ||
        name.contains("retainAutoreleasedReturnValue") ||
        name == "objc_opt_self") {
      if (call.getType()->isVoidTy())
        return true;
      const RegisterValue destination = registers_.at(&call);
      const auto source = registerFor(call.getArgOperand(0), destination.type);
      if (!source)
        return fail(call, "cannot lower retain/self operand");
      classOrigins_[destination.index] = classOrigins_[source->index];
      return emitResult(call, hfir::Opcode::Move, {reg(*source)});
    }
    if (name.ends_with("CMa") || name.contains("_builtinStringLiteral"))
      return true;
    if (name.contains("_bridgeToObjectiveC")) {
      const auto source = registerFor(call.getArgOperand(0),
                                      hfir::ValueType::String);
      return source ? emitResult(call, hfir::Opcode::Move, {reg(*source)})
                    : fail(call, "cannot lower Swift string bridge");
    }
    if (name.contains("SS1poi") || name.contains("StringV1poi")) {
      if (call.arg_size() < 2)
        return fail(call, "Swift string concatenation is missing operands");
      const unsigned rightIndex = call.arg_size() >= 4 ? 2 : 1;
      const auto left = registerFor(call.getArgOperand(0),
                                    hfir::ValueType::String);
      const auto right = registerFor(call.getArgOperand(rightIndex),
                                     hfir::ValueType::String);
      return left && right
                 ? emitResult(call, hfir::Opcode::StringConcat,
                              {reg(*left), reg(*right)})
                 : fail(call, "cannot lower Swift string concatenation");
    }
    if (name.contains("C5frameABSo6CGRectV_tcfC"))
      return lowerFrameConstructor(call, name);
    if (name == "objc_msgSend")
      return lowerObjCMessage(call);
    if (name.contains("assertionFailure") &&
        isa<UnreachableInst>(call.getParent()->getTerminator()))
      return true;
    if (!callee->isDeclaration()) {
      const auto local = functionIndices_.find(callee);
      if (local != functionIndices_.end())
        return lowerLocalCall(call, *callee, local->second);
    }
    return lowerNativeCall(call, *callee);
  }

  bool lowerLocalCall(CallBase &call, Function &callee,
                      std::uint32_t functionIndex) {
    if (call.arg_size() != callee.arg_size())
      return fail(call, "local helper argument count mismatch");
    std::vector<hfir::Operand> operands = {functionOperand(functionIndex)};
    auto parameter = callee.arg_begin();
    for (Value *argument : call.args()) {
      const auto type = typeForLLVM(parameter->getType());
      if (!type || *type == hfir::ValueType::Void)
        return fail(call, "local helper parameter has no HFIR representation");
      const auto lowered = registerFor(argument, *type);
      if (!lowered)
        return fail(call, "cannot lower local helper argument");
      operands.push_back(reg(*lowered));
      ++parameter;
    }
    if (callee.getReturnType()->isVoidTy()) {
      emitEffect(hfir::Opcode::FunctionCall, std::move(operands));
      return true;
    }
    return emitResult(call, hfir::Opcode::FunctionCall, std::move(operands));
  }

  bool lowerNativeCall(CallBase &call, Function &callee) {
    const StringRef symbol = callee.getName();
    if (callee.isVarArg())
      return fail(call, "variadic native calls are unsupported");
    if (call.hasOperandBundles())
      return fail(call, "native call operand bundles are unsupported");
    hfir::HostImportKind kind;
    std::string owner;
    std::optional<unsigned> receiverIndex;

    if (symbol.starts_with("$s") ||
        call.getCallingConv() == CallingConv::Swift ||
        call.getCallingConv() == CallingConv::SwiftTail) {
      kind = hfir::HostImportKind::NativeSwift;
      owner = "Swift";
      for (unsigned index = 0; index < call.arg_size(); ++index) {
        if (call.paramHasAttr(index, Attribute::SwiftSelf)) {
          if (receiverIndex)
            return fail(call, "native Swift call has multiple receivers");
          receiverIndex = index;
        }
      }
    } else if (symbol.starts_with("_Z")) {
      kind = hfir::HostImportKind::NativeCXX;
      owner = "C++";
      const Attribute receiverAttribute =
          callee.getFnAttribute("irhotfix.receiver-index");
      if (receiverAttribute.isValid()) {
        unsigned explicitReceiver = 0;
        if (receiverAttribute.getValueAsString().getAsInteger(
                10, explicitReceiver) || explicitReceiver >= call.arg_size())
          return fail(call, "invalid explicit C++ receiver index");
        receiverIndex = explicitReceiver;
      }
    } else {
      if (call.getCallingConv() != CallingConv::C &&
          call.getCallingConv() != CallingConv::Fast &&
          call.getCallingConv() != CallingConv::Cold)
        return fail(call, "unsupported native calling convention");
      kind = hfir::HostImportKind::NativeC;
    }

    if (!call.getType()->isVoidTy() && !registers_.contains(&call))
      return fail(call, "native return type has no HFIR representation");
    const hfir::ValueType returnType = call.getType()->isVoidTy()
        ? hfir::ValueType::Void
        : registers_.at(&call).type;
    if (call.getType()->isIntegerTy() && !call.getType()->isIntegerTy(1) &&
        !call.getType()->isIntegerTy(64))
      return fail(call, "native integer return must be i1 or i64");

    std::vector<hfir::ValueType> parameterTypes;
    std::vector<hfir::Operand> operands;
    std::optional<RegisterValue> receiver;
    for (unsigned index = 0; index < call.arg_size(); ++index) {
      if (call.paramHasAttr(index, Attribute::StructRet) ||
          call.paramHasAttr(index, Attribute::ByVal) ||
          call.paramHasAttr(index, Attribute::InAlloca) ||
          call.paramHasAttr(index, Attribute::Preallocated) ||
          call.paramHasAttr(index, Attribute::SwiftError) ||
          call.paramHasAttr(index, Attribute::SwiftAsync))
        return fail(call, "native call uses an unsupported ABI parameter");
      Value *argumentValue = call.getArgOperand(index);
      if (argumentValue->getType()->isIntegerTy() &&
          !argumentValue->getType()->isIntegerTy(1) &&
          !argumentValue->getType()->isIntegerTy(64))
        return fail(call, "native integer argument must be i1 or i64");
      const auto type = semanticType(argumentValue);
      if (!type || *type == hfir::ValueType::Void)
        return fail(call, "native argument has no HFIR representation");
      const auto lowered = registerFor(argumentValue, *type);
      if (!lowered)
        return fail(call, "cannot lower native argument");
      if (receiverIndex && *receiverIndex == index) {
        if (*type != hfir::ValueType::Handle)
          return fail(call, "native receiver must be a host handle");
        receiver = *lowered;
      } else {
        parameterTypes.push_back(*type);
        operands.push_back(reg(*lowered));
      }
    }

    const bool hasReceiver = receiver.has_value();
    if (parameterTypes.size() > hfir::kMaximumHostArgumentCount)
      return fail(call, "native call exceeds maximum host argument count");
    const std::uint32_t import = hostImport(
        kind, owner, symbol, returnType, parameterTypes, hasReceiver,
        fnv1a64(symbol));
    std::vector<hfir::Operand> callOperands = {imported(import)};
    if (receiver)
      callOperands.push_back(reg(*receiver));
    callOperands.insert(callOperands.end(), operands.begin(), operands.end());
    if (returnType == hfir::ValueType::Void) {
      emitEffect(hfir::Opcode::HostCall, std::move(callOperands));
      return true;
    }
    return emitResult(call, hfir::Opcode::HostCall, std::move(callOperands));
  }

  bool lowerFrameConstructor(CallBase &call, StringRef name) {
    if (call.arg_size() < 5)
      return fail(call, "frame initializer has an unexpected ABI");
    std::vector<double> values;
    std::vector<RegisterValue> components;
    bool allConstant = true;
    for (unsigned index = 0; index < 4; ++index) {
      const auto *constant = dyn_cast<ConstantFP>(call.getArgOperand(index));
      if (constant == nullptr) {
        allConstant = false;
        const auto component = registerFor(call.getArgOperand(index),
                                           hfir::ValueType::F64);
        if (!component)
          return fail(call, "cannot lower dynamic CGRect component");
        components.push_back(*component);
      } else {
        values.push_back(constant->getValueAPF().convertToDouble());
        const auto component = registerFor(call.getArgOperand(index),
                                           hfir::ValueType::F64);
        if (!component)
          return fail(call, "cannot lower CGRect constant component");
        components.push_back(*component);
      }
    }
    RegisterValue rectangle;
    if (allConstant) {
      const std::vector<std::uint8_t> bytes =
          doubles({values[0], values[1], values[2], values[3]});
      rectangle = emitConstant(
          {hfir::ConstantKind::Rect, 0, bytes}, hfir::ValueType::Rect);
    } else {
      rectangle = assignTemporary(hfir::ValueType::Rect);
      current_->instructions.push_back(
          {hfir::Opcode::PackRect, rectangle.index, hfir::ValueType::Rect,
           {reg(components[0]), reg(components[1]), reg(components[2]),
            reg(components[3])}});
    }
    const auto objectClass =
        registerFor(call.getArgOperand(call.arg_size() - 1),
                    hfir::ValueType::Handle);
    const std::string className = importedSwiftClassName(name);
    if (!objectClass || className.empty())
      return fail(call, "cannot lower Objective-C frame initializer class");
    const std::uint32_t import = hostImport(
        hfir::HostImportKind::Constructor, className, "initWithFrame:",
        hfir::ValueType::Handle, {hfir::ValueType::Rect}, true);
    classOrigins_[registers_.at(&call).index] = className;
    return emitResult(call, hfir::Opcode::ObjectConstruct,
                      {imported(import), reg(*objectClass), reg(rectangle)});
  }

  bool lowerObjCMessage(CallBase &call) {
    if (call.arg_size() < 2)
      return fail(call, "objc_msgSend is missing receiver or selector");
    const std::string selector = selectorFromGlobal(call.getArgOperand(1));
    if (selector.empty())
      return fail(call, "objc_msgSend selector is not a selector global");
    const auto receiver = registerFor(call.getArgOperand(0),
                                      hfir::ValueType::Handle);
    if (!receiver)
      return fail(call, "cannot lower Objective-C receiver");
    std::vector<hfir::ValueType> parameterTypes;
    std::vector<hfir::Operand> operands;
    for (unsigned index = 2; index < call.arg_size(); ++index) {
      const auto type = semanticType(call.getArgOperand(index));
      if (!type || *type == hfir::ValueType::Void)
        return fail(call, "unsupported Objective-C argument type");
      const auto argument = registerFor(call.getArgOperand(index), *type);
      if (!argument)
        return fail(call, "cannot lower Objective-C argument");
      parameterTypes.push_back(*type);
      operands.push_back(reg(*argument));
    }
    const hfir::ValueType returnType =
        call.getType()->isVoidTy() ? hfir::ValueType::Void
                                   : registers_.at(&call).type;
    std::string owner = classOrigins_[receiver->index];
    if (owner.empty()) owner = "NSObject";
    const std::uint32_t import =
        hostImport(hfir::HostImportKind::Method, owner, selector, returnType,
                   parameterTypes, true);
    std::vector<hfir::Operand> callOperands = {imported(import), reg(*receiver)};
    callOperands.insert(callOperands.end(), operands.begin(), operands.end());
    if (returnType == hfir::ValueType::Void) {
      emitEffect(hfir::Opcode::ObjectInvoke, std::move(callOperands));
      return true;
    }
    if (selector == "view")
      classOrigins_[registers_.at(&call).index] = "UIView";
    return emitResult(call, hfir::Opcode::ObjectInvoke,
                      std::move(callOperands));
  }

  bool fail(const Twine &message) {
    *error_ = message.str();
    return false;
  }

  bool fail(const Instruction &instruction, const Twine &message) {
    std::string rendered;
    raw_string_ostream stream(rendered);
    instruction.print(stream);
    stream.flush();
    *error_ = "function '" + function_.getName().str() + "': " +
              message.str() + "\n  LLVM: " + rendered;
    return false;
  }

  Function &function_;
  hfir::Package &package_;
  std::uint32_t functionIndex_ = 0;
  const std::unordered_map<const Function *, std::uint32_t> &functionIndices_;
  hfir::Function output_;
  hfir::BasicBlock *current_ = nullptr;
  std::string *error_ = nullptr;
  std::unordered_map<Value *, RegisterValue> registers_;
  std::unordered_map<const AllocaInst *, std::uint32_t> locals_;
  std::unordered_map<const Value *, std::uint32_t> localPointers_;
  std::unordered_map<const AllocaInst *, std::vector<std::uint32_t>>
      allocationLocals_;
  std::set<const AllocaInst *> aggregateAllocations_;
  std::unordered_map<std::string, std::uint32_t> aggregateFieldLocals_;
  std::unordered_map<const BasicBlock *, std::uint32_t> blockIDs_;
  std::unordered_map<Value *, std::optional<hfir::ValueType>> semanticTypes_;
  std::unordered_map<Value *, bool> semanticVisiting_;
  std::unordered_map<std::string, std::uint32_t> importIndices_;
  std::unordered_map<std::uint32_t, std::string> classOrigins_;
  std::unordered_map<const CallBase *, RegisterValue> stringLiteralRegisters_;
};

} // namespace

bool lowerFunction(Module &module, Function &function, std::uint64_t targetID,
                   std::uint64_t signatureID, hfir::Package &package,
                   std::string &error) {
  std::vector<Function *> reachable;
  std::set<Function *> visited;
  std::vector<Function *> worklist = {&function};
  while (!worklist.empty()) {
    Function *current = worklist.back();
    worklist.pop_back();
    if (!visited.insert(current).second)
      continue;
    reachable.push_back(current);
    for (BasicBlock &block : *current) {
      for (Instruction &instruction : block) {
        auto *call = dyn_cast<CallBase>(&instruction);
        Function *callee = call == nullptr ? nullptr : directCallee(*call);
        if (callee != nullptr && !callee->isDeclaration() &&
            !callee->isIntrinsic() &&
            !callee->getName().contains("__ir_hotfix_patch_anchor_") &&
            (callee->hasLocalLinkage() ||
             callee->getName().starts_with("$s13IRHotfixPatch")))
          worklist.push_back(callee);
      }
    }
  }

  package = {};
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "hfir." + utohexstr(targetID, true, 16);
  package.target = {targetID, signatureID, 0};
  package.functions.resize(reachable.size());
  std::unordered_map<const Function *, std::uint32_t> functionIndices;
  for (std::size_t index = 0; index < reachable.size(); ++index)
    functionIndices.emplace(reachable[index], static_cast<std::uint32_t>(index));

  for (std::size_t index = 0; index < reachable.size(); ++index) {
    if (!FunctionLowerer(*reachable[index], package,
                         static_cast<std::uint32_t>(index), functionIndices)
             .run(error))
      return false;
  }
  std::string verificationError;
  if (!hfir::verify(package, verificationError)) {
    error = "generated invalid HFIR: " + verificationError;
    return false;
  }
  return true;
}

} // namespace irhotfix::lowering
