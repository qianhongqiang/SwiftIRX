#include "HFNativePatchRuntime.h"

#include "../HostHandle/HFHostHandleTable.h"

HFStatus hf_native_patch_invoke(
    uint64_t targetID,
    uint64_t signatureID,
    void *receiver,
    HFHandleKind receiverKind,
    const HFValue *arguments,
    uint32_t argumentCount,
    HFValue *result
) {
    HFPatchFrame frame = HFMakePatchFrame();
    frame.targetID = targetID;
    frame.signatureID = signatureID;
    frame.arguments = arguments;
    frame.argumentCount = argumentCount;

    HFStatus scopeStatus = HFStatusApplied;
    if (receiver != NULL) {
        if (receiverKind == HFHandleKindInvalid)
            return HFStatusInvalidFrame;
        frame.flags = HFPatchFrameFlagHasReceiver;
        scopeStatus = hf_host_handle_scope_begin(
            receiver,
            receiverKind,
            &frame.receiver
        );
        if (scopeStatus != HFStatusApplied)
            return scopeStatus;
    } else if (receiverKind != HFHandleKindInvalid) {
        return HFStatusInvalidFrame;
    }

    const HFStatus status = hf_vm_invoke(&frame);
    if (receiver != NULL)
        (void)hf_host_handle_scope_end(frame.receiver);
    if (result != NULL)
        *result = frame.result;
    return status;
}
