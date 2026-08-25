#include "HFIRRuntime.h"
#include "HFHostAdapter.hpp"
#include "HFPatchContainer.h"
#include "IRHotfixObjCBridge.h"

#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace irhotfix;

namespace {

[[noreturn]] void failed(const std::string &message) {
  std::cerr << "HFIRVMTests: " << message << '\n';
  std::exit(1);
}

void require(bool condition, const std::string &message) {
  if (!condition)
    failed(message);
}

void setTarget(hfir::Package &package, std::uint64_t targetID,
               hfir::TargetValueKind returnType,
               std::vector<hfir::TargetValueKind> parameterTypes = {},
               hfir::TargetReceiverKind receiverKind =
                   hfir::TargetReceiverKind::None) {
  package.target.targetID = targetID;
  package.target.entryFunction = 0;
  package.target.abi.returnType = returnType;
  package.target.abi.parameterTypes = std::move(parameterTypes);
  package.target.abi.receiverKind = receiverKind;
  package.target.signatureID = hfir::targetSignatureID(package.target.abi);
}

std::vector<std::uint8_t> encode(const hfir::Package &package) {
  std::vector<std::uint8_t> bytes;
  std::string error;
  require(container::encode(package, bytes, error), "encode failed: " + error);
  return bytes;
}

hfir::Package makeArithmeticPackage() {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.add-ten";
  setTarget(package, 0x1001, TargetValueKind::I64, {TargetValueKind::I64});
  package.constants = {{ConstantKind::I64, 10, {}}};

  Function function;
  function.name = "addTen";
  function.returnType = ValueType::I64;
  function.parameterTypes = {ValueType::I64};
  function.registerTypes = {ValueType::I64, ValueType::I64, ValueType::I64};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::Constant, 1, ValueType::I64,
           {{OperandKind::Constant, ValueType::I64, 0}}},
          {Opcode::AddI64, 2, ValueType::I64,
           {{OperandKind::Register, ValueType::I64, 0},
            {OperandKind::Register, ValueType::I64, 1}}},
          {Opcode::Return, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::I64, 2}}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeSwitchPhiPackage() {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.switch-phi";
  setTarget(package, 0x1010, TargetValueKind::I64, {TargetValueKind::I64});
  package.constants = {
      {ConstantKind::I64, 0, {}}, {ConstantKind::I64, 1, {}},
      {ConstantKind::I64, 10, {}}, {ConstantKind::I64, 20, {}},
      {ConstantKind::I64, 30, {}},
  };
  Function function;
  function.name = "switchPhi";
  function.returnType = ValueType::I64;
  function.parameterTypes = {ValueType::I64};
  function.registerTypes = {ValueType::I64, ValueType::I64};
  function.entryBlock = 0;
  function.blocks = {
      {0, {{Opcode::Switch, kNoRegister, ValueType::Void,
            {{OperandKind::Register, ValueType::I64, 0},
             {OperandKind::Block, ValueType::Void, 3},
             {OperandKind::Constant, ValueType::I64, 0},
             {OperandKind::Block, ValueType::Void, 1},
             {OperandKind::Constant, ValueType::I64, 1},
             {OperandKind::Block, ValueType::Void, 2}}}}},
      {1, {{Opcode::Branch, kNoRegister, ValueType::Void,
            {{OperandKind::Block, ValueType::Void, 4}}}}},
      {2, {{Opcode::Branch, kNoRegister, ValueType::Void,
            {{OperandKind::Block, ValueType::Void, 4}}}}},
      {3, {{Opcode::Branch, kNoRegister, ValueType::Void,
            {{OperandKind::Block, ValueType::Void, 4}}}}},
      {4, {{Opcode::Phi, 1, ValueType::I64,
            {{OperandKind::Constant, ValueType::I64, 2},
             {OperandKind::Block, ValueType::Void, 1},
             {OperandKind::Constant, ValueType::I64, 3},
             {OperandKind::Block, ValueType::Void, 2},
             {OperandKind::Constant, ValueType::I64, 4},
             {OperandKind::Block, ValueType::Void, 3}}},
           {Opcode::Return, kNoRegister, ValueType::Void,
            {{OperandKind::Register, ValueType::I64, 1}}}}},
  };
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeObjCPackage() {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.objc-descriptor";
  setTarget(package, 0x1002, TargetValueKind::Void);
  package.imports = {
      {1, HostImportKind::Class, "NSObject", "NSObject", "",
       ValueType::Handle, {}, false},
      {2, HostImportKind::Constructor, "NSObject", "init", "",
       ValueType::Handle, {}, true},
      {3, HostImportKind::Method, "NSObject", "description", "",
       ValueType::Handle, {}, true},
      {4, HostImportKind::Method, "NSObject", "zone", "",
       ValueType::Handle, {}, true},
  };

  Function function;
  function.name = "objcDescriptorRoundTrip";
  function.returnType = ValueType::Void;
  function.registerTypes = {ValueType::Handle, ValueType::Handle,
                            ValueType::Handle, ValueType::Handle};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::ObjectClass, 0, ValueType::Handle,
           {{OperandKind::Import, ValueType::Void, 0}}},
          {Opcode::ObjectConstruct, 1, ValueType::Handle,
           {{OperandKind::Import, ValueType::Void, 1},
            {OperandKind::Register, ValueType::Handle, 0}}},
          {Opcode::ObjectInvoke, 2, ValueType::Handle,
           {{OperandKind::Import, ValueType::Void, 2},
            {OperandKind::Register, ValueType::Handle, 1}}},
          {Opcode::ObjectInvoke, 3, ValueType::Handle,
           {{OperandKind::Import, ValueType::Void, 3},
            {OperandKind::Register, ValueType::Handle, 1}}},
          {Opcode::ObjectRelease, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::Handle, 3}}},
          {Opcode::ObjectRelease, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::Handle, 2}}},
          {Opcode::ObjectRelease, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::Handle, 1}}},
          {Opcode::Return, kNoRegister, ValueType::Void, {}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeNativeCPackage(const char *symbol) {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.native-c";
  setTarget(package, 0x1003, TargetValueKind::I64, {TargetValueKind::I64});
  package.imports = {{
      hf_host_call_id(symbol), HostImportKind::NativeC, "", symbol, "",
      ValueType::I64, {ValueType::I64}, false,
  }};
  Function function;
  function.name = "nativeC";
  function.returnType = ValueType::I64;
  function.parameterTypes = {ValueType::I64};
  function.registerTypes = {ValueType::I64, ValueType::I64};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::HostCall, 1, ValueType::I64,
           {{OperandKind::Import, ValueType::Void, 0},
            {OperandKind::Register, ValueType::I64, 0}}},
          {Opcode::Return, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::I64, 1}}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeNativeNullPackage(const char *symbol) {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.native-null";
  setTarget(package, 0x1004, TargetValueKind::Bool);
  package.constants = {{ConstantKind::NullHandle, 0, {}}};
  package.imports = {{
      hf_host_call_id(symbol), HostImportKind::NativeC, "", symbol, "",
      ValueType::Handle, {}, false,
  }};
  Function function;
  function.name = "nativeNull";
  function.returnType = ValueType::Bool;
  function.registerTypes = {
      ValueType::Handle, ValueType::Handle, ValueType::Bool};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::HostCall, 0, ValueType::Handle,
           {{OperandKind::Import, ValueType::Void, 0}}},
          {Opcode::Constant, 1, ValueType::Handle,
           {{OperandKind::Constant, ValueType::Handle, 0}}},
          {Opcode::CompareEqual, 2, ValueType::Bool,
           {{OperandKind::Register, ValueType::Handle, 0},
            {OperandKind::Register, ValueType::Handle, 1}}},
          {Opcode::Return, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::Bool, 2}}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeNativeNullArgumentPackage(const char *symbol) {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.native-null-argument";
  setTarget(package, 0x1005, TargetValueKind::Bool);
  package.constants = {{ConstantKind::NullHandle, 0, {}}};
  package.imports = {{
      hf_host_call_id(symbol), HostImportKind::NativeC, "", symbol, "",
      ValueType::Bool, {ValueType::Handle}, false,
  }};
  Function function;
  function.name = "nativeNullArgument";
  function.returnType = ValueType::Bool;
  function.registerTypes = {ValueType::Handle, ValueType::Bool};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::Constant, 0, ValueType::Handle,
           {{OperandKind::Constant, ValueType::Handle, 0}}},
          {Opcode::HostCall, 1, ValueType::Bool,
           {{OperandKind::Import, ValueType::Void, 0},
            {OperandKind::Register, ValueType::Handle, 0}}},
          {Opcode::Return, kNoRegister, ValueType::Void,
           {{OperandKind::Register, ValueType::Bool, 1}}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeNativePreflightPackage(const char *sideEffectSymbol,
                                         const char *missingSymbol) {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.native-preflight";
  setTarget(package, 0x1006, TargetValueKind::Void);
  package.imports = {
      {hf_host_call_id(sideEffectSymbol), HostImportKind::NativeC, "",
       sideEffectSymbol, "", ValueType::Void, {}, false},
      {hf_host_call_id(missingSymbol), HostImportKind::NativeC, "",
       missingSymbol, "", ValueType::Void, {}, false},
  };
  Function function;
  function.name = "nativePreflight";
  function.returnType = ValueType::Void;
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::HostCall, kNoRegister, ValueType::Void,
           {{OperandKind::Import, ValueType::Void, 0}}},
          {Opcode::HostCall, kNoRegister, ValueType::Void,
           {{OperandKind::Import, ValueType::Void, 1}}},
          {Opcode::Return, kNoRegister, ValueType::Void, {}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeNativeCommittedFailurePackage(const char *sideEffectSymbol) {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.native-committed-failure";
  setTarget(package, 0x1007, TargetValueKind::Void);
  package.constants = {
      {ConstantKind::I64, 1, {}},
      {ConstantKind::I64, 0, {}},
  };
  package.imports = {{
      hf_host_call_id(sideEffectSymbol), HostImportKind::NativeC, "",
      sideEffectSymbol, "", ValueType::Void, {}, false,
  }};
  Function function;
  function.name = "nativeCommittedFailure";
  function.returnType = ValueType::Void;
  function.registerTypes = {ValueType::I64, ValueType::I64, ValueType::I64};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {
          {Opcode::HostCall, kNoRegister, ValueType::Void,
           {{OperandKind::Import, ValueType::Void, 0}}},
          {Opcode::Constant, 0, ValueType::I64,
           {{OperandKind::Constant, ValueType::I64, 0}}},
          {Opcode::Constant, 1, ValueType::I64,
           {{OperandKind::Constant, ValueType::I64, 1}}},
          {Opcode::DivI64, 2, ValueType::I64,
           {{OperandKind::Register, ValueType::I64, 0},
            {OperandKind::Register, ValueType::I64, 1}}},
          {Opcode::Return, kNoRegister, ValueType::Void, {}},
      },
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeBoundaryIdentityPackage(
    hfir::TargetValueKind boundaryKind, std::uint64_t targetID) {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = boundaryKind == TargetValueKind::Object
                        ? "vm.object-boundary"
                        : "vm.string-boundary";
  setTarget(package, targetID, boundaryKind, {boundaryKind});
  const ValueType valueType = boundaryKind == TargetValueKind::Object
                                  ? ValueType::Handle
                                  : ValueType::String;
  Function function;
  function.name = "boundaryIdentity";
  function.returnType = valueType;
  function.parameterTypes = {valueType};
  function.registerTypes = {valueType};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {{Opcode::Return, kNoRegister, ValueType::Void,
        {{OperandKind::Register, valueType, 0}}}},
  }};
  package.functions = {std::move(function)};
  return package;
}

hfir::Package makeNativeReceiverPackage() {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.native-receiver";
  setTarget(package, 0x1103, TargetValueKind::I64, {TargetValueKind::I64},
            TargetReceiverKind::Native);
  Function function;
  function.name = "nativeReceiverIdentity";
  function.returnType = ValueType::I64;
  function.parameterTypes = {ValueType::Handle, ValueType::I64};
  function.registerTypes = {ValueType::Handle, ValueType::I64};
  function.entryBlock = 0;
  function.blocks = {{
      0,
      {{Opcode::Return, kNoRegister, ValueType::Void,
        {{OperandKind::Register, ValueType::I64, 1}}}},
  }};
  package.functions = {std::move(function)};
  return package;
}

HFIRPatchHandle installAndActivate(const hfir::Package &package) {
  const std::vector<std::uint8_t> bytes = encode(package);
  HFIRPatchHandle handle = {};
  require(hf_hfir_vm_install(bytes.data(), bytes.size(), &handle) ==
              HFStatusApplied,
          "install failed");
  require(hf_hfir_vm_activate(handle) == HFStatusApplied, "activate failed");
  return handle;
}

void testArithmeticAndFrameValidation() {
  const hfir::Package package = makeArithmeticPackage();
  const HFIRPatchHandle handle = installAndActivate(package);
  HFValue argument = HFMakeValue(HFValueKindSignedInteger, 32);
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  frame.arguments = &argument;
  frame.argumentCount = 1;
  require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
          "arithmetic invocation failed");
  require(frame.result.kind == HFValueKindSignedInteger &&
              frame.result.bits == 42,
          "arithmetic result is not 42");

  HFPatchFrame malformed = frame;
  malformed.flags = 1u << 31;
  require(hf_hfir_vm_invoke(&malformed) == HFStatusInvalidFrame,
          "unknown frame flags were accepted");

  require(hf_hfir_vm_deactivate(handle) == HFStatusApplied,
          "deactivate failed");
  frame.result = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_hfir_vm_invoke(&frame) == HFStatusNoPatch,
          "deactivated patch remained callable");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");
}

void testDescriptorDrivenObjCInvocation() {
  const hfir::Package package = makeObjCPackage();
  const HFIRPatchHandle handle = installAndActivate(package);
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
          "descriptor-driven Objective-C invocation failed");
  require(frame.result.kind == HFValueKindVoid,
          "Objective-C patch returned a non-void result");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");
}

void testObjCPreflightPreservesOffMainFallback() {
  const hfir::Package package = makeObjCPackage();
  const HFIRPatchHandle handle = installAndActivate(package);
  HFStatus status = HFStatusApplied;
  std::thread worker([&] {
    HFPatchFrame frame = HFMakePatchFrame();
    frame.targetID = package.target.targetID;
    frame.signatureID = package.target.signatureID;
    status = hf_hfir_vm_invoke(&frame);
  });
  worker.join();
  require(status == HFStatusExecutionFailed,
          "off-main Objective-C preflight incorrectly committed execution");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");
}

std::int64_t nativeAddTen(std::int64_t value) { return value + 10; }

std::uint64_t nativeUnsignedIdentity(std::uint64_t value) { return value; }

void *nativeNull() { return nullptr; }

bool nativeIsNull(void *value) { return value == nullptr; }

std::int64_t nativeSideEffectCount = 0;
void nativeSideEffect() { ++nativeSideEffectCount; }

HFStatus nativeVoidEntry(HFHostCallFrame *frame) {
  if (frame == nullptr)
    return HFStatusInvalidFrame;
  frame->result = HFMakeValue(HFValueKindVoid, 0);
  return HFStatusApplied;
}

HFStatus nativeInvalidRetainedEntry(HFHostCallFrame *frame) {
  if (frame == nullptr)
    return HFStatusInvalidFrame;
  frame->result = HFMakeValue(HFValueKindHostHandle, 1);
  frame->result.flags = HFValueFlagRetainedHostHandle;
  return HFStatusApplied;
}

HFStatus nativeContextIntegerEntry(HFHostCallFrame *frame) {
  if (frame == nullptr || frame->context == nullptr)
    return HFStatusInvalidFrame;
  frame->result = HFMakeValue(
      HFValueKindSignedInteger,
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
          frame->context)));
  return HFStatusApplied;
}

class NativeMultiplier {
public:
  explicit NativeMultiplier(std::int64_t multiplier)
      : multiplier_(multiplier) {}
  std::int64_t multiply(std::int64_t value) { return value * multiplier_; }

private:
  std::int64_t multiplier_;
};

void testNativeCVMGateway() {
  constexpr const char *symbol = "ir.tests.nativeAddTen";
  irhotfix::host::Registration registration;
  require(irhotfix::host::registerFunction(
              symbol, HFHostLanguageC, &nativeAddTen, registration) ==
              HFStatusApplied,
          "native C gateway registration failed");
  const hfir::Package package = makeNativeCPackage(symbol);
  const HFIRPatchHandle handle = installAndActivate(package);
  HFValue argument = HFMakeValue(HFValueKindSignedInteger, 32);
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  frame.arguments = &argument;
  frame.argumentCount = 1;
  require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
          "HFIR host.call did not reach the native C gateway");
  require(frame.result.kind == HFValueKindSignedInteger &&
              frame.result.bits == 42,
          "native C gateway returned the wrong value");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");

  constexpr const char *unsignedSymbol = "ir.tests.nativeUnsignedIdentity";
  irhotfix::host::Registration unsignedRegistration;
  require(irhotfix::host::registerFunction(
              unsignedSymbol, HFHostLanguageC, &nativeUnsignedIdentity,
              unsignedRegistration) == HFStatusApplied,
          "native UInt64 gateway registration failed");
  const hfir::Package unsignedPackage = makeNativeCPackage(unsignedSymbol);
  const HFIRPatchHandle unsignedHandle = installAndActivate(unsignedPackage);
  HFValue unsignedArgument =
      HFMakeValue(HFValueKindSignedInteger, UINT64_MAX);
  HFPatchFrame unsignedFrame = HFMakePatchFrame();
  unsignedFrame.targetID = unsignedPackage.target.targetID;
  unsignedFrame.signatureID = unsignedPackage.target.signatureID;
  unsignedFrame.arguments = &unsignedArgument;
  unsignedFrame.argumentCount = 1;
  require(hf_hfir_vm_invoke(&unsignedFrame) == HFStatusApplied,
          "canonical HFIR i64 did not reach a UInt64 gateway");
  require(unsignedFrame.result.kind == HFValueKindSignedInteger &&
              unsignedFrame.result.bits == UINT64_MAX,
          "UInt64 gateway did not preserve the i64 bit pattern");
  require(hf_hfir_vm_uninstall(unsignedHandle) == HFStatusApplied,
          "uninstall failed");
}

void testNativeNullHandleGateway() {
  constexpr const char *symbol = "ir.tests.nativeNull";
  irhotfix::host::Registration registration;
  require(irhotfix::host::registerFunction(
              symbol, HFHostLanguageC, &nativeNull, registration) ==
              HFStatusApplied,
          "native null gateway registration failed");
  const hfir::Package package = makeNativeNullPackage(symbol);
  const HFIRPatchHandle handle = installAndActivate(package);
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
          "null host handle did not cross the native gateway");
  require(frame.result.kind == HFValueKindBool && frame.result.bits == 1,
          "null host handle did not retain HFIR null semantics");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");

  constexpr const char *argumentSymbol = "ir.tests.nativeIsNull";
  irhotfix::host::Registration argumentRegistration;
  require(irhotfix::host::registerFunction(
              argumentSymbol, HFHostLanguageC, &nativeIsNull,
              argumentRegistration) == HFStatusApplied,
          "native null argument gateway registration failed");
  const hfir::Package argumentPackage =
      makeNativeNullArgumentPackage(argumentSymbol);
  const HFIRPatchHandle argumentHandle = installAndActivate(argumentPackage);
  HFPatchFrame argumentFrame = HFMakePatchFrame();
  argumentFrame.targetID = argumentPackage.target.targetID;
  argumentFrame.signatureID = argumentPackage.target.signatureID;
  require(hf_hfir_vm_invoke(&argumentFrame) == HFStatusApplied,
          "null host handle argument did not reach the native gateway");
  require(argumentFrame.result.kind == HFValueKindBool &&
              argumentFrame.result.bits == 1,
          "native gateway did not decode a null host handle argument");
  require(hf_hfir_vm_uninstall(argumentHandle) == HFStatusApplied,
          "uninstall failed");
}

void testNativeAdapterPreflightPreventsPartialEffects() {
  constexpr const char *sideEffectSymbol = "ir.tests.nativeSideEffect";
  constexpr const char *missingSymbol = "ir.tests.missingAdapter";
  irhotfix::host::Registration registration;
  require(irhotfix::host::registerFunction(
              sideEffectSymbol, HFHostLanguageC, &nativeSideEffect,
              registration) == HFStatusApplied,
          "native side-effect registration failed");
  const hfir::Package package =
      makeNativePreflightPackage(sideEffectSymbol, missingSymbol);
  const HFIRPatchHandle handle = installAndActivate(package);
  nativeSideEffectCount = 0;
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  require(hf_hfir_vm_invoke(&frame) == HFStatusExecutionFailed,
          "missing native adapter did not fail preflight");
  require(nativeSideEffectCount == 0,
          "native preflight allowed partial host side effects");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");
}

void testNativeFailureAfterEffectsSuppressesFallback() {
  constexpr const char *symbol = "ir.tests.nativeCommittedSideEffect";
  irhotfix::host::Registration registration;
  require(irhotfix::host::registerFunction(
              symbol, HFHostLanguageC, &nativeSideEffect, registration) ==
              HFStatusApplied,
          "committed side-effect registration failed");
  const hfir::Package package = makeNativeCommittedFailurePackage(symbol);
  const HFIRPatchHandle handle = installAndActivate(package);
  nativeSideEffectCount = 0;
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  require(hf_hfir_vm_invoke(&frame) == HFStatusExecutionCommitted,
          "post-effect VM fault was allowed to request native fallback");
  require(nativeSideEffectCount == 1,
          "committed side effect did not execute exactly once");
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied, "uninstall failed");
}

void testNativeRegistrationIdentityAndNullCallables() {
  constexpr const char *symbol = "ir.tests.registrationIdentity";
  HFHostCallDescriptor call = irhotfix::host::descriptor(
      symbol, HFHostLanguageC, HFHostCallKindFunction, HFValueKindVoid,
      nullptr, 0);
  HFHostAdapterRegistration first = HFInvalidHostAdapterRegistration();
  require(hf_host_adapter_register(&call, &nativeVoidEntry, nullptr, nullptr,
                                   &first) == HFStatusApplied,
          "first raw adapter registration failed");
  const HFHostAdapterRegistration stale = first;
  require(hf_host_adapter_unregister(first) == HFStatusApplied,
          "first raw adapter unregister failed");
  HFHostAdapterRegistration replacement = HFInvalidHostAdapterRegistration();
  require(hf_host_adapter_register(&call, &nativeVoidEntry, nullptr, nullptr,
                                   &replacement) == HFStatusApplied,
          "replacement raw adapter registration failed");
  require(hf_host_adapter_unregister(stale) == HFStatusHostAdapterNotFound,
          "stale unregister removed a replacement adapter");
  require(hf_host_adapter_validate(&call) == HFStatusApplied,
          "replacement adapter was not preserved after stale unregister");
  require(hf_host_adapter_unregister(replacement) == HFStatusApplied,
          "replacement raw adapter unregister failed");

  constexpr const char *retainedSymbol = "ir.tests.invalidRetainedPointer";
  HFHostCallDescriptor retainedCall = irhotfix::host::descriptor(
      retainedSymbol, HFHostLanguageC, HFHostCallKindFunction,
      HFValueKindHostHandle, nullptr, 0);
  HFHostAdapterRegistration retainedRegistration =
      HFInvalidHostAdapterRegistration();
  require(hf_host_adapter_register(
              &retainedCall, &nativeInvalidRetainedEntry, nullptr, nullptr,
              &retainedRegistration) == HFStatusApplied,
          "invalid retained-pointer adapter registration failed");
  HFValue retainedResult = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_host_adapter_invoke(&retainedCall, HFInvalidHandle(), nullptr, 0,
                                 &retainedResult) == HFStatusInvalidResult,
          "native retained pointer without Objective-C ownership was accepted");
  require(hf_host_adapter_unregister(retainedRegistration) == HFStatusApplied,
          "retained-pointer adapter unregister failed");

  irhotfix::host::Registration nullFunctionRegistration;
  std::int64_t (*nullFunction)(std::int64_t) = nullptr;
  require(irhotfix::host::registerFunction(
              "ir.tests.nullFunction", HFHostLanguageC, nullFunction,
              nullFunctionRegistration) == HFStatusInvalidArguments,
          "null native function was registered");
  irhotfix::host::Registration nullMethodRegistration;
  std::int64_t (NativeMultiplier::*nullMethod)(std::int64_t) = nullptr;
  require(irhotfix::host::registerMethod(
              "ir.tests.nullMethod", nullMethod,
              nullMethodRegistration) == HFStatusInvalidArguments,
          "null native method was registered");
}

void testNativeLeasePinsExactRegistration() {
  constexpr const char *symbol = "ir.tests.leaseIdentity";
  HFHostCallDescriptor call = irhotfix::host::descriptor(
      symbol, HFHostLanguageC, HFHostCallKindFunction,
      HFValueKindSignedInteger, nullptr, 0);
  HFHostAdapterRegistration first = HFInvalidHostAdapterRegistration();
  require(hf_host_adapter_register(
              &call, &nativeContextIntegerEntry,
              reinterpret_cast<void *>(static_cast<std::uintptr_t>(1)),
              nullptr, &first) == HFStatusApplied,
          "leased adapter registration failed");
  HFHostAdapterLease *lease = nullptr;
  require(hf_host_adapter_acquire(&call, &lease) == HFStatusApplied &&
              lease != nullptr,
          "adapter lease acquisition failed");
  require(hf_host_adapter_unregister(first) == HFStatusApplied,
          "leased adapter unregister failed");

  HFHostAdapterRegistration replacement = HFInvalidHostAdapterRegistration();
  require(hf_host_adapter_register(
              &call, &nativeContextIntegerEntry,
              reinterpret_cast<void *>(static_cast<std::uintptr_t>(2)),
              nullptr, &replacement) == HFStatusApplied,
          "replacement adapter registration failed");
  HFValue leasedResult = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_host_adapter_invoke_leased(
              lease, HFInvalidHandle(), nullptr, 0, &leasedResult) ==
              HFStatusApplied &&
              leasedResult.bits == 1,
          "lease did not pin the exact original adapter context");
  HFValue currentResult = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_host_adapter_invoke(
              &call, HFInvalidHandle(), nullptr, 0, &currentResult) ==
              HFStatusApplied &&
              currentResult.bits == 2,
          "one-shot invocation did not resolve the replacement adapter");
  hf_host_adapter_release(lease);
  require(hf_host_adapter_unregister(replacement) == HFStatusApplied,
          "replacement adapter unregister failed");
}

void testNativeCXXMethodGateway() {
  constexpr const char *symbol = "ir.tests.NativeMultiplier.multiply";
  irhotfix::host::Registration registration;
  require(irhotfix::host::registerMethod(
              symbol, &NativeMultiplier::multiply, registration) ==
              HFStatusApplied,
          "native C++ method registration failed");
  constexpr HFValueKind argumentKinds[] = {HFValueKindSignedInteger};
  HFHostCallDescriptor call = irhotfix::host::descriptor(
      symbol, HFHostLanguageCXX, HFHostCallKindInstanceMethod,
      HFValueKindSignedInteger, argumentKinds, 1, true);
  NativeMultiplier multiplier(3);
  HFHandle receiver = HFInvalidHandle();
  require(hf_host_handle_scope_begin(&multiplier, HFHandleKindNativeSymbol,
                                     &receiver) == HFStatusApplied,
          "native C++ receiver handle registration failed");
  HFValue argument = HFMakeValue(HFValueKindSignedInteger, 14);
  HFValue result = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_host_adapter_invoke(&call, receiver, &argument, 1, &result) ==
              HFStatusApplied,
          "native C++ method gateway invocation failed");
  require(result.kind == HFValueKindSignedInteger && result.bits == 42,
          "native C++ method gateway returned the wrong value");
  require(hf_host_handle_scope_end(receiver) == HFStatusApplied,
          "native C++ receiver handle release failed");
}

void testSwitchAndConstantPhi() {
  const hfir::Package package = makeSwitchPhiPackage();
  const HFIRPatchHandle handle = installAndActivate(package);
  for (const auto &[input, expected] :
       std::vector<std::pair<std::uint64_t, std::uint64_t>>{
           {0, 10}, {1, 20}, {9, 30}}) {
    HFValue argument = HFMakeValue(HFValueKindSignedInteger, input);
    HFPatchFrame frame = HFMakePatchFrame();
    frame.targetID = package.target.targetID;
    frame.signatureID = package.target.signatureID;
    frame.arguments = &argument;
    frame.argumentCount = 1;
    require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
            "switch/phi package invocation failed");
    require(frame.result.kind == HFValueKindSignedInteger &&
                frame.result.bits == expected,
            "switch/phi package returned the wrong branch");
  }
  require(hf_hfir_vm_uninstall(handle) == HFStatusApplied,
          "switch/phi package uninstall failed");
}

void testHostHandleGenerationOwnershipAndLease() {
  const std::uint64_t baseline = hf_host_handle_live_count();
  int native = 42;
  HFHandle first = HFInvalidHandle();
  require(hf_host_handle_scope_begin(&native, HFHandleKindNativeSymbol,
                                     &first) == HFStatusApplied,
          "borrowed host handle registration failed");
  HFHostHandleLease *lease = nullptr;
  void *resolved = nullptr;
  require(hf_host_handle_resolve(first, &lease, &resolved) == HFStatusApplied &&
              resolved == &native,
          "borrowed host handle resolution failed");
  require(hf_host_handle_scope_end(first) == HFStatusApplied,
          "borrowed host handle release failed");
  require(hf_host_handle_validate(first) == HFStatusStaleHandle,
          "released host handle was not stale");
  require(resolved == &native,
          "active invocation lease lost its resolved value");
  hf_host_handle_lease_release(lease);

  HFHandle second = HFInvalidHandle();
  require(hf_host_handle_scope_begin(&native, HFHandleKindNativeSymbol,
                                     &second) == HFStatusApplied,
          "reused host handle registration failed");
  require(second.token == first.token && second.generation != first.generation,
          "reused handle slot did not advance generation");
  require(hf_host_handle_scope_end(second) == HFStatusApplied,
          "reused host handle release failed");

  void *string = IRHFObjCCreateStringUTF8("handle", 6);
  require(string != nullptr, "test string allocation failed");
  HFHandle strong = HFInvalidHandle();
  require(hf_host_handle_register(string, HFHandleKindObject,
                                  HFHostHandleOwnershipStrong,
                                  &strong) == HFStatusApplied,
          "strong object handle registration failed");
  IRHFObjCReleaseRetainedObject(string);
  require(hf_host_handle_validate(strong) == HFStatusApplied,
          "strong object handle did not retain its object");
  require(hf_host_handle_release(strong) == HFStatusApplied,
          "strong object handle release failed");

  const std::string weakBytes(128, 'w');
  void *weakString = IRHFObjCCreateStringUTF8(weakBytes.data(), weakBytes.size());
  require(weakString != nullptr, "weak test string allocation failed");
  HFHandle weak = HFInvalidHandle();
  require(hf_host_handle_register(weakString, HFHandleKindObject,
                                  HFHostHandleOwnershipWeak,
                                  &weak) == HFStatusApplied,
          "weak object handle registration failed");
  IRHFObjCReleaseRetainedObject(weakString);
  require(hf_host_handle_validate(weak) == HFStatusStaleHandle,
          "deallocated weak object handle was not stale");
  require(hf_host_handle_release(weak) == HFStatusApplied,
          "stale weak object handle entry could not be released");
  require(hf_host_handle_live_count() == baseline,
          "host handle table leaked a live entry");
}

void testObjectAndStringTargetBoundaries() {
  const std::uint64_t baseline = hf_host_handle_live_count();
  void *string = IRHFObjCCreateStringUTF8("boundary", 8);
  require(string != nullptr, "boundary string allocation failed");
  HFHandle borrowed = HFInvalidHandle();
  require(hf_host_handle_scope_begin(string, HFHandleKindObject, &borrowed) ==
              HFStatusApplied,
          "boundary object scope registration failed");

  const hfir::Package objectPackage = makeBoundaryIdentityPackage(
      hfir::TargetValueKind::Object, 0x1101);
  const HFIRPatchHandle objectPatch = installAndActivate(objectPackage);
  HFValue objectArgument = HFValueFromHostHandle(
      borrowed, HFValueFlagBorrowedHostHandle);
  HFPatchFrame objectFrame = HFMakePatchFrame();
  objectFrame.targetID = objectPackage.target.targetID;
  objectFrame.signatureID = objectPackage.target.signatureID;
  objectFrame.arguments = &objectArgument;
  objectFrame.argumentCount = 1;
  require(hf_hfir_vm_invoke(&objectFrame) == HFStatusApplied,
          "object target boundary invocation failed");
  HFHandle objectResult = HFInvalidHandle();
  require(objectFrame.result.kind == HFValueKindHostHandle &&
              objectFrame.result.flags == HFValueFlagRetainedHostHandle &&
              HFValueGetHostHandle(&objectFrame.result, &objectResult),
          "object target boundary did not return a transferred handle");
  require(hf_host_handle_release(objectResult) == HFStatusApplied,
          "object boundary result handle release failed");
  require(hf_hfir_vm_uninstall(objectPatch) == HFStatusApplied,
          "object boundary patch uninstall failed");

  const hfir::Package stringPackage = makeBoundaryIdentityPackage(
      hfir::TargetValueKind::String, 0x1102);
  const HFIRPatchHandle stringPatch = installAndActivate(stringPackage);
  HFValue stringArgument = HFValueFromStringHandle(
      borrowed, HFValueFlagBorrowedHostHandle);
  HFPatchFrame stringFrame = HFMakePatchFrame();
  stringFrame.targetID = stringPackage.target.targetID;
  stringFrame.signatureID = stringPackage.target.signatureID;
  stringFrame.arguments = &stringArgument;
  stringFrame.argumentCount = 1;
  require(hf_hfir_vm_invoke(&stringFrame) == HFStatusApplied,
          "string target boundary invocation failed");
  HFHandle stringResult = HFInvalidHandle();
  require(stringFrame.result.kind == HFValueKindStringHandle &&
              stringFrame.result.flags == HFValueFlagRetainedHostHandle &&
              HFValueGetStringHandle(&stringFrame.result, &stringResult),
          "string target boundary did not preserve its distinct tag");
  require(hf_host_handle_release(stringResult) == HFStatusApplied,
          "string boundary result handle release failed");

  HFPatchFrame wrongTag = HFMakePatchFrame();
  wrongTag.targetID = stringPackage.target.targetID;
  wrongTag.signatureID = stringPackage.target.signatureID;
  wrongTag.arguments = &objectArgument;
  wrongTag.argumentCount = 1;
  require(hf_hfir_vm_invoke(&wrongTag) == HFStatusExecutionFailed,
          "generic object handle was accepted as a String boundary value");

  IRHFObjCInvocationResult objectCreation = IRHFObjCConstruct(
      IRHFObjCLookUpClass("NSObject"), "init", nullptr, 0);
  require(objectCreation.status == IRHFObjCInvocationStatusSuccess &&
              objectCreation.value.kind == IRHFValueKindObject &&
              objectCreation.value.bits != 0,
          "non-string boundary object allocation failed");
  void *plainObject = reinterpret_cast<void *>(
      static_cast<std::uintptr_t>(objectCreation.value.bits));
  HFHandle plainObjectHandle = HFInvalidHandle();
  require(hf_host_handle_scope_begin(plainObject, HFHandleKindObject,
                                     &plainObjectHandle) == HFStatusApplied,
          "non-string boundary handle registration failed");
  HFValue forgedString = HFValueFromStringHandle(
      plainObjectHandle, HFValueFlagBorrowedHostHandle);
  HFPatchFrame wrongObject = stringFrame;
  wrongObject.arguments = &forgedString;
  wrongObject.result = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_hfir_vm_invoke(&wrongObject) == HFStatusExecutionFailed,
          "non-NSString object was accepted behind a String handle tag");
  require(hf_host_handle_scope_end(plainObjectHandle) == HFStatusApplied,
          "non-string boundary handle release failed");
  IRHFObjCReleaseRetainedObject(plainObject);

  require(hf_hfir_vm_uninstall(stringPatch) == HFStatusApplied,
          "string boundary patch uninstall failed");
  require(hf_host_handle_scope_end(borrowed) == HFStatusApplied,
          "boundary object scope release failed");
  IRHFObjCReleaseRetainedObject(string);
  require(hf_host_handle_live_count() == baseline,
          "object/String boundary leaked a handle-table entry");
}

void testNativeTargetReceiverBoundary() {
  const hfir::Package package = makeNativeReceiverPackage();
  const HFIRPatchHandle patch = installAndActivate(package);
  std::int64_t receiverStorage = 0;
  HFHandle nativeReceiver = HFInvalidHandle();
  require(hf_host_handle_scope_begin(&receiverStorage, HFHandleKindNativeSymbol,
                                     &nativeReceiver) == HFStatusApplied,
          "native receiver registration failed");
  HFValue argument = HFMakeValue(HFValueKindSignedInteger, 42);
  HFPatchFrame frame = HFMakePatchFrame();
  frame.targetID = package.target.targetID;
  frame.signatureID = package.target.signatureID;
  frame.flags = HFPatchFrameFlagHasReceiver;
  frame.receiver = nativeReceiver;
  frame.arguments = &argument;
  frame.argumentCount = 1;
  require(hf_hfir_vm_invoke(&frame) == HFStatusApplied &&
              frame.result.kind == HFValueKindSignedInteger &&
              frame.result.bits == 42,
          "native target receiver invocation failed");

  IRHFObjCInvocationResult objectCreation = IRHFObjCConstruct(
      IRHFObjCLookUpClass("NSObject"), "init", nullptr, 0);
  require(objectCreation.status == IRHFObjCInvocationStatusSuccess &&
              objectCreation.value.kind == IRHFValueKindObject,
          "receiver mismatch object allocation failed");
  void *object = reinterpret_cast<void *>(
      static_cast<std::uintptr_t>(objectCreation.value.bits));
  HFHandle objectReceiver = HFInvalidHandle();
  require(hf_host_handle_scope_begin(object, HFHandleKindObject,
                                     &objectReceiver) == HFStatusApplied,
          "object receiver registration failed");
  frame.receiver = objectReceiver;
  frame.result = HFMakeValue(HFValueKindInvalid, 0);
  require(hf_hfir_vm_invoke(&frame) == HFStatusInvalidFrame,
          "native target accepted an Objective-C object handle");
  require(hf_host_handle_scope_end(objectReceiver) == HFStatusApplied,
          "object receiver release failed");
  IRHFObjCReleaseRetainedObject(object);
  require(hf_host_handle_scope_end(nativeReceiver) == HFStatusApplied,
          "native receiver release failed");
  require(hf_hfir_vm_uninstall(patch) == HFStatusApplied,
          "native receiver patch uninstall failed");
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 5 &&
      (std::string(argv[1]) == "--invoke-i64" ||
       std::string(argv[1]) == "--invoke-native-i64")) {
    const bool hasNativeReceiver =
        std::string(argv[1]) == "--invoke-native-i64";
    std::ifstream input(argv[2], std::ios::binary);
    require(input.good(), "cannot open patch for invocation");
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    HFIRPatchHandle handle = {};
    require(hf_hfir_vm_install(bytes.data(), bytes.size(), &handle) ==
                HFStatusApplied,
            "patch runner install failed");
    require(hf_hfir_vm_activate(handle) == HFStatusApplied,
            "patch runner activation failed");
    HFValue argument = HFMakeValue(
        HFValueKindSignedInteger,
        static_cast<std::uint64_t>(std::stoll(argv[3])));
    HFPatchFrame frame = HFMakePatchFrame();
    frame.targetID = handle.targetID;
    frame.signatureID = handle.signatureID;
    frame.arguments = &argument;
    frame.argumentCount = 1;
    std::int64_t receiverStorage = 0;
    if (hasNativeReceiver) {
      frame.flags = HFPatchFrameFlagHasReceiver;
      require(hf_host_handle_scope_begin(&receiverStorage,
                                         HFHandleKindNativeSymbol,
                                         &frame.receiver) == HFStatusApplied,
              "patch runner native receiver registration failed");
    }
    require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
            "patch runner invocation failed");
    require(frame.result.kind == HFValueKindSignedInteger &&
                static_cast<std::int64_t>(frame.result.bits) ==
                    std::stoll(argv[4]),
            "patch runner returned an unexpected value");
    if (hasNativeReceiver)
      require(hf_host_handle_scope_end(frame.receiver) == HFStatusApplied,
              "patch runner native receiver release failed");
    require(hf_hfir_vm_uninstall(handle) == HFStatusApplied,
            "patch runner uninstall failed");
    std::cout << "HFIR patch invocation passed\n";
    return 0;
  }
  if (argc == 5 && std::string(argv[1]) == "--invoke-f64") {
    std::ifstream input(argv[2], std::ios::binary);
    require(input.good(), "cannot open floating patch for invocation");
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    HFIRPatchHandle handle = {};
    require(hf_hfir_vm_install(bytes.data(), bytes.size(), &handle) ==
                HFStatusApplied,
            "floating patch runner install failed");
    require(hf_hfir_vm_activate(handle) == HFStatusApplied,
            "floating patch runner activation failed");
    const double argumentNumber = std::stod(argv[3]);
    HFValue argument = HFMakeValue(
        HFValueKindFloat64, std::bit_cast<std::uint64_t>(argumentNumber));
    HFPatchFrame frame = HFMakePatchFrame();
    frame.targetID = handle.targetID;
    frame.signatureID = handle.signatureID;
    frame.arguments = &argument;
    frame.argumentCount = 1;
    require(hf_hfir_vm_invoke(&frame) == HFStatusApplied,
            "floating patch runner invocation failed");
    const double result = std::bit_cast<double>(frame.result.bits);
    require(frame.result.kind == HFValueKindFloat64 &&
                std::abs(result - std::stod(argv[4])) < 0.0001,
            "floating patch runner returned an unexpected value");
    require(hf_hfir_vm_uninstall(handle) == HFStatusApplied,
            "floating patch runner uninstall failed");
    std::cout << "HFIR floating patch invocation passed\n";
    return 0;
  }
  testArithmeticAndFrameValidation();
  testDescriptorDrivenObjCInvocation();
  testObjCPreflightPreservesOffMainFallback();
  testNativeCVMGateway();
  testNativeNullHandleGateway();
  testNativeAdapterPreflightPreventsPartialEffects();
  testNativeFailureAfterEffectsSuppressesFallback();
  testNativeRegistrationIdentityAndNullCallables();
  testNativeLeasePinsExactRegistration();
  testNativeCXXMethodGateway();
  testSwitchAndConstantPhi();
  testHostHandleGenerationOwnershipAndLease();
  testObjectAndStringTargetBoundaries();
  testNativeTargetReceiverBoundary();
  std::cout << "HFIRVMTests passed\n";
  return 0;
}
