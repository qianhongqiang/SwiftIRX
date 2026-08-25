#include "HotfixNativeExamples.h"

#include "../../SDK/IRHotfixSDK/Runtime/HFNativePatchRuntime.h"

static const uint64_t HFExampleCTargetID = UINT64_C(0x509615878e53c0ac);
static const uint64_t HFExampleCSignatureID = UINT64_C(0xa41bdf07a2f121e1);

__attribute__((noinline, used, visibility("default")))
int64_t hotfix_example_c_add(int64_t value) {
    const HFValue argument = HFMakeValue(
        HFValueKindSignedInteger,
        (uint64_t)value
    );
    HFValue result = HFMakeValue(HFValueKindInvalid, 0);
    const HFStatus status = hf_native_patch_invoke(
        HFExampleCTargetID,
        HFExampleCSignatureID,
        NULL,
        HFHandleKindInvalid,
        &argument,
        1,
        &result
    );
    if (status == HFStatusApplied && result.kind == HFValueKindSignedInteger)
        return (int64_t)result.bits;
    if (status == HFStatusExecutionCommitted)
        __builtin_trap();
    return value + 2;
}
