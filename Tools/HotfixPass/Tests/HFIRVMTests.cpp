#include "HFIRRuntime.h"
#include "HFPatchContainer.h"

#include <cstdlib>
#include <iostream>
#include <string>
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
  package.target = {0x1001, 0x2001, 0};
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

hfir::Package makeObjCPackage() {
  using namespace hfir;
  Package package;
  package.abiVersion = HF_ABI_VERSION;
  package.patchID = "vm.objc-descriptor";
  package.target = {0x1002, 0x2002, 0};
  package.imports = {
      {1, HostImportKind::Class, "NSObject", "NSObject", "",
       ValueType::Handle, {}, false},
      {2, HostImportKind::Constructor, "NSObject", "init", "",
       ValueType::Handle, {}, true},
      {3, HostImportKind::Method, "NSObject", "description", "",
       ValueType::Handle, {}, true},
  };

  Function function;
  function.name = "objcDescriptorRoundTrip";
  function.returnType = ValueType::Void;
  function.registerTypes = {ValueType::Handle, ValueType::Handle,
                            ValueType::Handle};
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

} // namespace

int main() {
  testArithmeticAndFrameValidation();
  testDescriptorDrivenObjCInvocation();
  std::cout << "HFIRVMTests passed\n";
  return 0;
}
