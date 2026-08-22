#include "HFHostAdapter.h"

#include "../Bridge/IRHotfixObjCBridge.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr HFHostCallFlags kKnownCallFlags =
    HFHostCallFlagHasReceiver | HFHostCallFlagMainThreadOnly |
    HFHostCallFlagObjCCompatibleHandles | HFHostCallFlagNoSideEffects;

std::uint64_t appendFNV(std::uint64_t hash, const void *bytes,
                        std::size_t byteCount) {
  const auto *cursor = static_cast<const std::uint8_t *>(bytes);
  for (std::size_t index = 0; index < byteCount; ++index) {
    hash ^= cursor[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t appendU32(std::uint64_t hash, std::uint32_t value) {
  std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(value),
      static_cast<std::uint8_t>(value >> 8),
      static_cast<std::uint8_t>(value >> 16),
      static_cast<std::uint8_t>(value >> 24),
  };
  return appendFNV(hash, bytes, sizeof(bytes));
}

bool validValueKind(HFValueKind kind, bool allowVoid) {
  switch (kind) {
  case HFValueKindSignedInteger:
  case HFValueKindUnsignedInteger:
  case HFValueKindBool:
  case HFValueKindFloat32:
  case HFValueKindFloat64:
  case HFValueKindMemoryOffset:
  case HFValueKindHostHandle:
  case HFValueKindBytes:
    return true;
  case HFValueKindVoid:
    return allowVoid;
  default:
    return false;
  }
}

bool validDescriptor(const HFHostCallDescriptor *descriptor,
                     bool requireSignature) {
  if (descriptor == nullptr ||
      descriptor->abiVersion != HF_HOST_ADAPTER_ABI_VERSION ||
      descriptor->structSize != sizeof(HFHostCallDescriptor) ||
      descriptor->importID == 0 || descriptor->reserved != 0 ||
      descriptor->name == nullptr || descriptor->name[0] == '\0' ||
      descriptor->argumentCount > HF_MAX_HOST_ARGUMENT_COUNT ||
      (descriptor->argumentCount != 0 && descriptor->argumentKinds == nullptr) ||
      (descriptor->flags & ~kKnownCallFlags) != 0)
    return false;
  if (descriptor->language < HFHostLanguageObjectiveC ||
      descriptor->language > HFHostLanguageCXX ||
      descriptor->callKind < HFHostCallKindFunction ||
      descriptor->callKind > HFHostCallKindClassLookup ||
      !validValueKind(descriptor->returnKind, true))
    return false;
  for (std::uint32_t index = 0; index < descriptor->argumentCount; ++index) {
    if (!validValueKind(descriptor->argumentKinds[index], false))
      return false;
  }
  if ((descriptor->flags & HFHostCallFlagHasReceiver) == 0 &&
      (descriptor->callKind == HFHostCallKindInstanceMethod ||
       descriptor->callKind == HFHostCallKindConstructor))
    return false;
  if (descriptor->callKind == HFHostCallKindClassLookup &&
      ((descriptor->flags & HFHostCallFlagHasReceiver) != 0 ||
       descriptor->argumentCount != 0 ||
       descriptor->returnKind != HFValueKindHostHandle))
    return false;
  return !requireSignature ||
         (descriptor->signatureID != 0 &&
          descriptor->signatureID == hf_host_call_signature_id(descriptor));
}

bool validHandle(HFHandle handle, bool required) {
  if (!required)
    return handle.token == 0 && handle.generation == 0 &&
           handle.kind == HFHandleKindInvalid &&
           handle.flags == HFHandleFlagNone;
  return handle.token != 0 && handle.generation == 0 &&
         (handle.kind == HFHandleKindObject ||
          handle.kind == HFHandleKindClass ||
          handle.kind == HFHandleKindNativeSymbol) &&
         (handle.flags == (HFHandleFlagBorrowed | HFHandleFlagBorrowedAddress) ||
          handle.flags == HFHandleFlagRetained);
}

bool validValue(const HFValue &value, HFValueKind expected, bool result) {
  if (value.kind != expected)
    return false;
  if (expected == HFValueKindHostHandle) {
    if (value.bits == 0)
      return value.flags == HFValueFlagNone && value.bytes == nullptr &&
             value.byteCount == 0;
    const bool validOwnership = result
        ? value.flags == HFValueFlagBorrowedHostHandle ||
              value.flags == HFValueFlagRetainedHostHandle
        : value.flags == HFValueFlagBorrowedHostHandle;
    return value.bytes == nullptr && value.byteCount == 0 && validOwnership;
  }
  if (expected == HFValueKindBytes)
    return value.flags == HFValueFlagNone &&
           (value.byteCount == 0 || value.bytes != nullptr);
  if (value.flags != HFValueFlagNone || value.bytes != nullptr ||
      value.byteCount != 0)
    return false;
  return expected != HFValueKindBool || value.bits <= 1;
}

void discardResult(HFValue &value, bool objcCompatibleHandles) {
  if (objcCompatibleHandles && value.kind == HFValueKindHostHandle &&
      value.bits != 0 && value.flags == HFValueFlagRetainedHostHandle) {
    IRHFObjCReleaseRetainedObject(reinterpret_cast<void *>(
        static_cast<std::uintptr_t>(value.bits)));
  }
  value = HFMakeValue(HFValueKindInvalid, 0);
}

struct OwnedDescriptor {
  HFHostCallDescriptor value = {};
  std::string owner;
  std::string name;
  std::string encoding;
  std::vector<HFValueKind> arguments;

  static std::vector<HFValueKind> copyArguments(
      const HFHostCallDescriptor &source) {
    if (source.argumentCount == 0)
      return {};
    return {source.argumentKinds,
            source.argumentKinds + source.argumentCount};
  }

  explicit OwnedDescriptor(const HFHostCallDescriptor &source)
      : value(source), owner(source.owner == nullptr ? "" : source.owner),
        name(source.name),
        encoding(source.typeEncoding == nullptr ? "" : source.typeEncoding),
        arguments(copyArguments(source)) {
    value.owner = owner.c_str();
    value.name = name.c_str();
    value.typeEncoding = encoding.c_str();
    value.argumentKinds = arguments.empty() ? nullptr : arguments.data();
  }
};

struct RegisteredAdapter {
  std::uint64_t token = 0;
  OwnedDescriptor descriptor;
  HFHostAdapterEntry entry = nullptr;
  void *context = nullptr;
  HFHostAdapterContextRelease releaseContext = nullptr;

  RegisteredAdapter(std::uint64_t token, const HFHostCallDescriptor &source,
                    HFHostAdapterEntry entry, void *context,
                    HFHostAdapterContextRelease releaseContext)
      : token(token), descriptor(source), entry(entry), context(context),
        releaseContext(releaseContext) {}

  ~RegisteredAdapter() {
    if (releaseContext != nullptr && context != nullptr)
      releaseContext(context);
  }
};

class Registry {
public:
  static Registry &shared() {
    static Registry registry;
    return registry;
  }

  HFStatus registerAdapter(const HFHostCallDescriptor &descriptor,
                           HFHostAdapterEntry entry, void *context,
                           HFHostAdapterContextRelease releaseContext,
                           HFHostAdapterRegistration &registration) {
    if (entry == nullptr || !validDescriptor(&descriptor, true))
      return HFStatusInvalidArguments;
    std::lock_guard lock(mutex_);
    if (adapters_.contains(descriptor.importID))
      return HFStatusHostAdapterConflict;
    std::uint64_t token = nextToken_.fetch_add(1, std::memory_order_relaxed);
    if (token == 0)
      token = nextToken_.fetch_add(1, std::memory_order_relaxed);
    auto adapter = std::make_shared<RegisteredAdapter>(
        token, descriptor, entry, context, nullptr);
    adapters_.emplace(descriptor.importID, std::move(adapter));
    adapters_.at(descriptor.importID)->releaseContext = releaseContext;
    registration = {token, descriptor.importID, descriptor.signatureID};
    return HFStatusApplied;
  }

  HFStatus unregisterAdapter(HFHostAdapterRegistration registration) {
    if (registration.token == 0 || registration.importID == 0 ||
        registration.signatureID == 0)
      return HFStatusInvalidArguments;
    std::shared_ptr<RegisteredAdapter> removed;
    {
      std::lock_guard lock(mutex_);
      const auto found = adapters_.find(registration.importID);
      if (found == adapters_.end())
        return HFStatusHostAdapterNotFound;
      if (found->second->token != registration.token)
        return HFStatusHostAdapterNotFound;
      if (found->second->descriptor.value.signatureID !=
          registration.signatureID)
        return HFStatusSignatureMismatch;
      removed = std::move(found->second);
      adapters_.erase(found);
    }
    return HFStatusApplied;
  }

  std::shared_ptr<RegisteredAdapter> find(std::uint64_t importID) {
    std::lock_guard lock(mutex_);
    const auto found = adapters_.find(importID);
    return found == adapters_.end() ? nullptr : found->second;
  }

private:
  std::mutex mutex_;
  std::atomic<std::uint64_t> nextToken_{1};
  std::unordered_map<std::uint64_t, std::shared_ptr<RegisteredAdapter>> adapters_;
};

HFStatus invokeObjectiveC(const HFHostCallDescriptor &descriptor,
                          HFHandle receiver, const HFValue *arguments,
                          HFValue &result) {
  if ((descriptor.flags & HFHostCallFlagMainThreadOnly) != 0 &&
      !IRHFObjCIsMainThread())
    return HFStatusExecutionFailed;
  if (descriptor.callKind == HFHostCallKindClassLookup) {
    void *objectClass = IRHFObjCLookUpClass(descriptor.owner);
    if (objectClass == nullptr)
      return HFStatusExecutionFailed;
    result = HFMakeValue(
        HFValueKindHostHandle,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(objectClass)));
    result.flags = HFValueFlagBorrowedHostHandle;
    return HFStatusApplied;
  }

  std::vector<IRHFValue> bridged(descriptor.argumentCount);
  for (std::uint32_t index = 0; index < descriptor.argumentCount; ++index) {
    const HFValue &source = arguments[index];
    IRHFValue &destination = bridged[index];
    destination.bits = source.bits;
    destination.bytes = source.bytes;
    destination.byteCount = source.byteCount;
    switch (source.kind) {
    case HFValueKindSignedInteger:
      destination.kind = IRHFValueKindSignedInteger;
      break;
    case HFValueKindUnsignedInteger:
      destination.kind = IRHFValueKindUnsignedInteger;
      break;
    case HFValueKindBool:
      destination.kind = IRHFValueKindBool;
      break;
    case HFValueKindFloat32:
      destination.kind = IRHFValueKindFloat32;
      break;
    case HFValueKindFloat64:
      destination.kind = IRHFValueKindFloat64;
      break;
    case HFValueKindHostHandle:
      destination.kind = IRHFValueKindObject;
      break;
    case HFValueKindBytes:
      destination.kind = IRHFValueKindBytes;
      break;
    default:
      return HFStatusInvalidArguments;
    }
  }

  void *object = reinterpret_cast<void *>(
      static_cast<std::uintptr_t>(receiver.token));
  const IRHFObjCInvocationResult invocation =
      descriptor.callKind == HFHostCallKindConstructor
          ? IRHFObjCConstruct(object, descriptor.name, bridged.data(),
                              bridged.size())
          : IRHFObjCInvoke(object, descriptor.name, bridged.data(),
                           bridged.size());
  if (invocation.status != IRHFObjCInvocationStatusSuccess)
    return HFStatusExecutionFailed;
  result = HFMakeValue(HFValueKindInvalid, invocation.value.bits);
  result.bytes = invocation.value.bytes;
  result.byteCount = invocation.value.byteCount;
  switch (invocation.value.kind) {
  case IRHFValueKindSignedInteger:
  case IRHFValueKindUnsignedInteger:
    result.kind = descriptor.returnKind == HFValueKindUnsignedInteger
        ? HFValueKindUnsignedInteger
        : HFValueKindSignedInteger;
    break;
  case IRHFValueKindBool: result.kind = HFValueKindBool; break;
  case IRHFValueKindFloat32: result.kind = HFValueKindFloat32; break;
  case IRHFValueKindFloat64: result.kind = HFValueKindFloat64; break;
  case IRHFValueKindObject:
    result.kind = HFValueKindHostHandle;
    result.flags = result.bits == 0 ? HFValueFlagNone
                                    : HFValueFlagRetainedHostHandle;
    break;
  case IRHFValueKindPointer:
    result.kind = HFValueKindHostHandle;
    result.flags = result.bits == 0 ? HFValueFlagNone
                                    : HFValueFlagBorrowedHostHandle;
    break;
  case IRHFValueKindBytes: result.kind = HFValueKindBytes; break;
  case IRHFValueKindVoid: result.kind = HFValueKindVoid; break;
  default: return HFStatusInvalidResult;
  }
  return HFStatusApplied;
}

HFStatus validateRegisteredAdapter(
    const HFHostCallDescriptor &descriptor,
    const std::shared_ptr<RegisteredAdapter> &adapter) {
  if (!adapter)
    return HFStatusHostAdapterNotFound;
  if (adapter->descriptor.value.signatureID != descriptor.signatureID ||
      adapter->descriptor.value.language != descriptor.language ||
      adapter->descriptor.value.callKind != descriptor.callKind)
    return HFStatusSignatureMismatch;
  if ((adapter->descriptor.value.flags & HFHostCallFlagMainThreadOnly) != 0 &&
      !IRHFObjCIsMainThread())
    return HFStatusExecutionFailed;
  return HFStatusApplied;
}

HFStatus invokeRegisteredAdapter(
    const std::shared_ptr<RegisteredAdapter> &adapter, HFHandle receiver,
    const HFValue *arguments, std::uint32_t argumentCount, HFValue *result) {
  if (!adapter || result == nullptr)
    return HFStatusInvalidArguments;
  const HFHostCallDescriptor &descriptor = adapter->descriptor.value;
  if (argumentCount != descriptor.argumentCount ||
      (argumentCount == 0) != (arguments == nullptr))
    return HFStatusInvalidArguments;
  const bool hasReceiver =
      (descriptor.flags & HFHostCallFlagHasReceiver) != 0;
  if (!validHandle(receiver, hasReceiver))
    return HFStatusInvalidArguments;
  for (std::uint32_t index = 0; index < argumentCount; ++index) {
    if (!validValue(arguments[index], descriptor.argumentKinds[index], false))
      return HFStatusInvalidArguments;
  }
  if ((descriptor.flags & HFHostCallFlagMainThreadOnly) != 0 &&
      !IRHFObjCIsMainThread())
    return HFStatusExecutionFailed;

  HFHostCallFrame frame = {};
  frame.abiVersion = HF_HOST_ADAPTER_ABI_VERSION;
  frame.structSize = sizeof(HFHostCallFrame);
  frame.descriptor = &descriptor;
  frame.receiver = receiver;
  frame.arguments = arguments;
  frame.argumentCount = argumentCount;
  frame.result = HFMakeValue(HFValueKindInvalid, 0);
  frame.context = adapter->context;
  frame.status = HFStatusInvalidFrame;
  HFStatus status = HFStatusExecutionFailed;
  try {
    status = adapter->entry(&frame);
  } catch (...) {
    status = HFStatusExecutionFailed;
  }
  frame.status = status;
  const bool objcCompatibleHandles =
      (descriptor.flags & HFHostCallFlagObjCCompatibleHandles) != 0;
  if (status != HFStatusApplied) {
    discardResult(frame.result, objcCompatibleHandles);
    return status;
  }
  if (!validValue(frame.result, descriptor.returnKind, true)) {
    discardResult(frame.result, objcCompatibleHandles);
    return HFStatusInvalidResult;
  }
  if (frame.result.kind == HFValueKindHostHandle &&
      frame.result.flags == HFValueFlagRetainedHostHandle &&
      !objcCompatibleHandles) {
    frame.result = HFMakeValue(HFValueKindInvalid, 0);
    return HFStatusInvalidResult;
  }
  *result = frame.result;
  return HFStatusApplied;
}

} // namespace

struct HFHostAdapterLease {
  std::shared_ptr<RegisteredAdapter> adapter;
};

uint64_t hf_host_call_id(const char *symbol) {
  if (symbol == nullptr || symbol[0] == '\0')
    return 0;
  return appendFNV(14695981039346656037ULL, symbol, std::strlen(symbol));
}

uint64_t hf_host_call_signature_id(const HFHostCallDescriptor *descriptor) {
  if (!validDescriptor(descriptor, false))
    return 0;
  std::uint64_t hash = 14695981039346656037ULL;
  hash = appendU32(hash, descriptor->language);
  hash = appendU32(hash, descriptor->callKind);
  hash = appendU32(hash, descriptor->returnKind);
  hash = appendU32(hash, descriptor->flags & HFHostCallFlagHasReceiver);
  hash = appendU32(hash, descriptor->argumentCount);
  for (std::uint32_t index = 0; index < descriptor->argumentCount; ++index)
    hash = appendU32(hash, descriptor->argumentKinds[index]);
  return hash;
}

HFStatus hf_host_adapter_register(
    const HFHostCallDescriptor *descriptor, HFHostAdapterEntry entry,
    void *context, HFHostAdapterContextRelease releaseContext,
    HFHostAdapterRegistration *registration) {
  if (registration != nullptr)
    *registration = HFInvalidHostAdapterRegistration();
  if (descriptor == nullptr || registration == nullptr)
    return HFStatusInvalidArguments;
  try {
    return Registry::shared().registerAdapter(*descriptor, entry, context,
                                              releaseContext, *registration);
  } catch (...) {
    return HFStatusExecutionFailed;
  }
}

HFStatus hf_host_adapter_unregister(HFHostAdapterRegistration registration) {
  return Registry::shared().unregisterAdapter(registration);
}

HFStatus hf_host_adapter_validate(const HFHostCallDescriptor *descriptor) {
  if (!validDescriptor(descriptor, true))
    return HFStatusInvalidArguments;
  if (descriptor->language == HFHostLanguageObjectiveC)
    return (descriptor->flags & HFHostCallFlagMainThreadOnly) == 0 ||
                   IRHFObjCIsMainThread()
               ? HFStatusApplied
               : HFStatusExecutionFailed;
  const auto adapter = Registry::shared().find(descriptor->importID);
  return validateRegisteredAdapter(*descriptor, adapter);
}

HFStatus hf_host_adapter_acquire(const HFHostCallDescriptor *descriptor,
                                 HFHostAdapterLease **lease) {
  if (lease != nullptr)
    *lease = nullptr;
  if (!validDescriptor(descriptor, true) || lease == nullptr ||
      descriptor->language == HFHostLanguageObjectiveC)
    return HFStatusInvalidArguments;
  try {
    const auto adapter = Registry::shared().find(descriptor->importID);
    const HFStatus status = validateRegisteredAdapter(*descriptor, adapter);
    if (status != HFStatusApplied)
      return status;
    *lease = new HFHostAdapterLease{adapter};
    return HFStatusApplied;
  } catch (...) {
    return HFStatusExecutionFailed;
  }
}

void hf_host_adapter_release(HFHostAdapterLease *lease) { delete lease; }

HFStatus hf_host_adapter_invoke_leased(
    HFHostAdapterLease *lease, HFHandle receiver, const HFValue *arguments,
    uint32_t argumentCount, HFValue *result) {
  return lease == nullptr
      ? HFStatusInvalidArguments
      : invokeRegisteredAdapter(lease->adapter, receiver, arguments,
                                argumentCount, result);
}

HFHostCallFlags hf_host_adapter_lease_flags(const HFHostAdapterLease *lease) {
  return lease == nullptr || !lease->adapter
      ? HFHostCallFlagNone
      : lease->adapter->descriptor.value.flags;
}

HFStatus hf_host_adapter_invoke(
    const HFHostCallDescriptor *descriptor, HFHandle receiver,
    const HFValue *arguments, uint32_t argumentCount, HFValue *result) {
  if (!validDescriptor(descriptor, true) || result == nullptr ||
      argumentCount != descriptor->argumentCount ||
      (argumentCount == 0) != (arguments == nullptr))
    return HFStatusInvalidArguments;
  const bool hasReceiver =
      (descriptor->flags & HFHostCallFlagHasReceiver) != 0;
  if (!validHandle(receiver, hasReceiver))
    return HFStatusInvalidArguments;
  for (std::uint32_t index = 0; index < argumentCount; ++index) {
    if (!validValue(arguments[index], descriptor->argumentKinds[index], false))
      return HFStatusInvalidArguments;
  }

  *result = HFMakeValue(HFValueKindInvalid, 0);
  if (descriptor->language == HFHostLanguageObjectiveC) {
    const HFStatus status =
        invokeObjectiveC(*descriptor, receiver, arguments, *result);
    if (status == HFStatusApplied &&
        !validValue(*result, descriptor->returnKind, true)) {
      discardResult(*result, true);
      return HFStatusInvalidResult;
    }
    if (status != HFStatusApplied)
      discardResult(*result, true);
    return status;
  }

  const auto adapter = Registry::shared().find(descriptor->importID);
  const HFStatus validation = validateRegisteredAdapter(*descriptor, adapter);
  if (validation != HFStatusApplied)
    return validation;
  const HFStatus status = invokeRegisteredAdapter(
      adapter, receiver, arguments, argumentCount, result);
  if (status == HFStatusApplied &&
      (result->kind == HFValueKindBytes ||
       (result->kind == HFValueKindHostHandle &&
        result->flags == HFValueFlagBorrowedHostHandle))) {
    *result = HFMakeValue(HFValueKindInvalid, 0);
    return HFStatusInvalidResult;
  }
  return status;
}
