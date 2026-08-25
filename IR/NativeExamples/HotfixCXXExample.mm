#include "HotfixNativeExamples.h"

#include "../../SDK/IRHotfixSDK/Runtime/HFNativePatchRuntime.h"

namespace {
constexpr uint64_t HFExampleCXXTargetID = UINT64_C(0x377fcf21d0d94ffc);
constexpr uint64_t HFExampleCXXSignatureID = UINT64_C(0xa41bde07a2f1202e);
}

class HFCalculator final {
public:
    __attribute__((noinline, used, visibility("default")))
    long long multiply(long long value);
};

long long HFCalculator::multiply(long long value) {
    const HFValue argument = HFMakeValue(
        HFValueKindSignedInteger,
        static_cast<uint64_t>(value)
    );
    HFValue result = HFMakeValue(HFValueKindInvalid, 0);
    const HFStatus status = hf_native_patch_invoke(
        HFExampleCXXTargetID,
        HFExampleCXXSignatureID,
        this,
        HFHandleKindNativeSymbol,
        &argument,
        1,
        &result
    );
    if (status == HFStatusApplied && result.kind == HFValueKindSignedInteger)
        return static_cast<long long>(result.bits);
    if (status == HFStatusExecutionCommitted)
        __builtin_trap();
    return value * 2;
}

extern "C" int64_t hotfix_example_cxx_multiply(int64_t value) {
    HFCalculator calculator;
    return static_cast<int64_t>(
        calculator.multiply(static_cast<long long>(value))
    );
}
