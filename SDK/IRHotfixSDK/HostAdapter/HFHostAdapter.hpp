#ifndef IRHotfixSDK_HFHostAdapter_hpp
#define IRHotfixSDK_HFHostAdapter_hpp

#include "HFHostAdapter.h"

#include <array>
#include <bit>
#include <cstdint>
#include <tuple>
#include <typeinfo>
#include <type_traits>
#include <utility>

namespace irhotfix::host {

template <typename T> struct ValueCodec;

template <> struct ValueCodec<std::int64_t> {
  static constexpr HFValueKind kind = HFValueKindSignedInteger;
  static bool decode(const HFValue &value, std::int64_t &output) {
    output = static_cast<std::int64_t>(value.bits);
    return value.kind == kind;
  }
  static HFValue encode(std::int64_t value) {
    return HFMakeValue(kind, static_cast<std::uint64_t>(value));
  }
};

template <> struct ValueCodec<std::uint64_t> {
  // HFIR i64 is an untyped 64-bit bit pattern and uses the signed host kind as
  // its canonical descriptor identity. Encoding preserves every UInt64 bit.
  static constexpr HFValueKind kind = HFValueKindSignedInteger;
  static bool decode(const HFValue &value, std::uint64_t &output) {
    output = value.bits;
    return value.kind == kind;
  }
  static HFValue encode(std::uint64_t value) {
    return HFMakeValue(kind, value);
  }
};

template <> struct ValueCodec<bool> {
  static constexpr HFValueKind kind = HFValueKindBool;
  static bool decode(const HFValue &value, bool &output) {
    output = value.bits != 0;
    return value.kind == kind && value.bits <= 1;
  }
  static HFValue encode(bool value) { return HFMakeValue(kind, value ? 1 : 0); }
};

template <> struct ValueCodec<float> {
  static constexpr HFValueKind kind = HFValueKindFloat32;
  static bool decode(const HFValue &value, float &output) {
    output = std::bit_cast<float>(static_cast<std::uint32_t>(value.bits));
    return value.kind == kind;
  }
  static HFValue encode(float value) {
    return HFMakeValue(kind, std::bit_cast<std::uint32_t>(value));
  }
};

template <> struct ValueCodec<double> {
  static constexpr HFValueKind kind = HFValueKindFloat64;
  static bool decode(const HFValue &value, double &output) {
    output = std::bit_cast<double>(value.bits);
    return value.kind == kind;
  }
  static HFValue encode(double value) {
    return HFMakeValue(kind, std::bit_cast<std::uint64_t>(value));
  }
};

template <> struct ValueCodec<void *> {
  static constexpr HFValueKind kind = HFValueKindHostHandle;
  static bool decode(const HFValue &value, void *&output) {
    output = reinterpret_cast<void *>(static_cast<std::uintptr_t>(value.bits));
    return value.kind == kind;
  }
  static HFValue encode(void *value) {
    HFValue encoded = HFMakeValue(
        kind, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)));
    encoded.flags = value == nullptr ? HFValueFlagNone
                                     : HFValueFlagBorrowedHostHandle;
    return encoded;
  }
};

template <> struct ValueCodec<const void *> {
  static constexpr HFValueKind kind = HFValueKindHostHandle;
  static bool decode(const HFValue &value, const void *&output) {
    output = reinterpret_cast<const void *>(
        static_cast<std::uintptr_t>(value.bits));
    return value.kind == kind;
  }
  static HFValue encode(const void *value) {
    HFValue encoded = HFMakeValue(
        kind, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)));
    encoded.flags = value == nullptr ? HFValueFlagNone
                                     : HFValueFlagBorrowedHostHandle;
    return encoded;
  }
};

template <typename T>
using Canonical = std::remove_cv_t<std::remove_reference_t<T>>;

class Registration {
public:
  Registration() = default;
  explicit Registration(HFHostAdapterRegistration registration)
      : registration_(registration) {}
  Registration(const Registration &) = delete;
  Registration &operator=(const Registration &) = delete;
  Registration(Registration &&other) noexcept
      : registration_(std::exchange(
            other.registration_, HFInvalidHostAdapterRegistration())) {}
  Registration &operator=(Registration &&other) noexcept {
    if (this != &other) {
      reset();
      registration_ = std::exchange(
          other.registration_, HFInvalidHostAdapterRegistration());
    }
    return *this;
  }
  ~Registration() { reset(); }

  explicit operator bool() const { return registration_.token != 0; }
  void reset() {
    if (registration_.token != 0)
      (void)hf_host_adapter_unregister(registration_);
    registration_ = HFInvalidHostAdapterRegistration();
  }

private:
  HFHostAdapterRegistration registration_ = HFInvalidHostAdapterRegistration();
};

inline HFHostCallDescriptor descriptor(const char *symbol,
                                       HFHostLanguage language,
                                       HFHostCallKind callKind,
                                       HFValueKind returnKind,
                                       const HFValueKind *argumentKinds,
                                       std::uint32_t argumentCount,
                                       bool hasReceiver = false,
                                       const char *owner = "",
                                       bool noSideEffects = false) {
  HFHostCallDescriptor value = {};
  value.abiVersion = HF_HOST_ADAPTER_ABI_VERSION;
  value.structSize = sizeof(HFHostCallDescriptor);
  value.importID = hf_host_call_id(symbol);
  value.language = language;
  value.callKind = callKind;
  value.returnKind = returnKind;
  value.argumentCount = argumentCount;
  value.flags = hasReceiver ? HFHostCallFlagHasReceiver : HFHostCallFlagNone;
  if (noSideEffects)
    value.flags |= HFHostCallFlagNoSideEffects;
  value.owner = owner;
  value.name = symbol;
  value.typeEncoding = "";
  value.argumentKinds = argumentKinds;
  value.signatureID = hf_host_call_signature_id(&value);
  return value;
}

namespace detail {

template <typename Result> constexpr HFValueKind resultKind() {
  if constexpr (std::is_void_v<Result>)
    return HFValueKindVoid;
  else
    return ValueCodec<Canonical<Result>>::kind;
}

template <typename Result, typename... Arguments> struct FunctionContext {
  Result (*function)(Arguments...);
};

template <typename... Arguments, std::size_t... Indices>
bool decodeArguments(const HFHostCallFrame &frame,
                     std::tuple<Canonical<Arguments>...> &decoded,
                     std::index_sequence<Indices...>) {
  return (ValueCodec<Canonical<Arguments>>::decode(
              frame.arguments[Indices], std::get<Indices>(decoded)) && ...);
}

template <typename Result, typename... Arguments>
HFStatus functionEntry(HFHostCallFrame *frame) {
  if (frame == nullptr || frame->context == nullptr ||
      frame->argumentCount != sizeof...(Arguments))
    return HFStatusInvalidFrame;
  auto *context = static_cast<FunctionContext<Result, Arguments...> *>(
      frame->context);
  std::tuple<Canonical<Arguments>...> decoded;
  if (!decodeArguments<Arguments...>(
          *frame, decoded, std::index_sequence_for<Arguments...>{}))
    return HFStatusInvalidArguments;
  if constexpr (std::is_void_v<Result>) {
    std::apply(context->function, decoded);
    frame->result = HFMakeValue(HFValueKindVoid, 0);
  } else {
    frame->result = ValueCodec<Canonical<Result>>::encode(
        std::apply(context->function, decoded));
  }
  return HFStatusApplied;
}

template <typename Context> void deleteContext(void *context) {
  delete static_cast<Context *>(context);
}

template <typename Class, typename Result, typename... Arguments>
struct MethodContext {
  Result (Class::*method)(Arguments...);
};

template <typename Class, typename Result, typename... Arguments>
HFStatus methodEntry(HFHostCallFrame *frame) {
  if (frame == nullptr || frame->context == nullptr ||
      frame->receiver.token == 0 ||
      frame->argumentCount != sizeof...(Arguments))
    return HFStatusInvalidFrame;
  auto *context = static_cast<MethodContext<Class, Result, Arguments...> *>(
      frame->context);
  auto *object = reinterpret_cast<Class *>(
      static_cast<std::uintptr_t>(frame->receiver.token));
  std::tuple<Canonical<Arguments>...> decoded;
  if (!decodeArguments<Arguments...>(
          *frame, decoded, std::index_sequence_for<Arguments...>{}))
    return HFStatusInvalidArguments;
  if constexpr (std::is_void_v<Result>) {
    std::apply([&](auto &&...values) { (object->*context->method)(values...); },
               decoded);
    frame->result = HFMakeValue(HFValueKindVoid, 0);
  } else {
    const Result result = std::apply(
        [&](auto &&...values) { return (object->*context->method)(values...); },
        decoded);
    frame->result = ValueCodec<Canonical<Result>>::encode(result);
  }
  return HFStatusApplied;
}

} // namespace detail

template <typename Result, typename... Arguments>
HFStatus registerFunction(const char *symbol, HFHostLanguage language,
                          Result (*function)(Arguments...),
                          Registration &registration,
                          bool noSideEffects = false) {
  if (function == nullptr)
    return HFStatusInvalidArguments;
  static constexpr std::array<HFValueKind, sizeof...(Arguments)> argumentKinds = {
      ValueCodec<Canonical<Arguments>>::kind...};
  using Context = detail::FunctionContext<Result, Arguments...>;
  auto *context = new Context{function};
  HFHostCallDescriptor call = descriptor(
      symbol, language, HFHostCallKindFunction,
      detail::resultKind<Result>(), argumentKinds.data(),
      sizeof...(Arguments), false, "", noSideEffects);
  HFHostAdapterRegistration registered = HFInvalidHostAdapterRegistration();
  const HFStatus status = hf_host_adapter_register(
      &call, &detail::functionEntry<Result, Arguments...>, context,
      &detail::deleteContext<Context>, &registered);
  if (status == HFStatusApplied)
    registration = Registration(registered);
  else
    delete context;
  return status;
}

template <typename Class, typename Result, typename... Arguments>
HFStatus registerMethod(const char *symbol, Result (Class::*method)(Arguments...),
                        Registration &registration,
                        bool noSideEffects = false) {
  if (method == nullptr)
    return HFStatusInvalidArguments;
  static constexpr std::array<HFValueKind, sizeof...(Arguments)> argumentKinds = {
      ValueCodec<Canonical<Arguments>>::kind...};
  using Context = detail::MethodContext<Class, Result, Arguments...>;
  auto *context = new Context{method};
  HFHostCallDescriptor call = descriptor(
      symbol, HFHostLanguageCXX, HFHostCallKindInstanceMethod,
      detail::resultKind<Result>(), argumentKinds.data(),
      sizeof...(Arguments), true, typeid(Class).name(), noSideEffects);
  HFHostAdapterRegistration registered = HFInvalidHostAdapterRegistration();
  const HFStatus status = hf_host_adapter_register(
      &call, &detail::methodEntry<Class, Result, Arguments...>, context,
      &detail::deleteContext<Context>, &registered);
  if (status == HFStatusApplied)
    registration = Registration(registered);
  else
    delete context;
  return status;
}

} // namespace irhotfix::host

#endif /* IRHotfixSDK_HFHostAdapter_hpp */
