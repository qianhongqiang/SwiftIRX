#ifndef HFPatchFrame_h
#define HFPatchFrame_h

#include <stdint.h>

#include "HFHandle.h"
#include "HFStatus.h"
#include "HFValue.h"

#define HF_ABI_VERSION 1u
#define HF_MAX_SCALAR_ARGUMENT_COUNT 8u

typedef uint32_t HFPatchFrameFlags;
enum {
    HFPatchFrameFlagNone = 0,
    HFPatchFrameFlagHasReceiver = 1u << 0,
};

typedef struct HFPatchFrame {
    uint32_t abiVersion;
    uint32_t structSize;
    uint64_t targetID;
    uint64_t signatureID;
    const HFValue *arguments;
    uint32_t argumentCount;
    HFPatchFrameFlags flags;
    HFHandle receiver;
    HFValue result;
    HFStatus status;
    uint32_t reserved;
} HFPatchFrame;

static inline HFPatchFrame HFMakePatchFrame(void) {
    HFPatchFrame frame = {0};
    frame.abiVersion = HF_ABI_VERSION;
    frame.structSize = (uint32_t)sizeof(HFPatchFrame);
    frame.receiver = HFInvalidHandle();
    frame.result = HFMakeValue(HFValueKindInvalid, 0);
    frame.status = HFStatusInvalidFrame;
    return frame;
}

#if defined(__cplusplus)
extern "C" {
#endif

HFStatus hf_vm_invoke(HFPatchFrame *frame);

#if defined(__cplusplus)
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(HFPatchFrame) == 96, "HFPatchFrame ABI layout changed");
#else
_Static_assert(sizeof(HFPatchFrame) == 96, "HFPatchFrame ABI layout changed");
#endif

#endif /* HFPatchFrame_h */
