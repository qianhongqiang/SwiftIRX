#ifndef IRHotfixSDK_HFNativePatchRuntime_h
#define IRHotfixSDK_HFNativePatchRuntime_h

#include "../ABI/HFPatchFrame.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Descriptor-driven entry point for C functions and non-virtual C++ methods.
/// `receiver` is null for C functions. C++ methods pass `this` with
/// HFHandleKindNativeSymbol; the pointer is exposed to HFIR only as a scoped,
/// generation-checked handle.
HFStatus hf_native_patch_invoke(
    uint64_t targetID,
    uint64_t signatureID,
    void *receiver,
    HFHandleKind receiverKind,
    const HFValue *arguments,
    uint32_t argumentCount,
    HFValue *result
);

#ifdef __cplusplus
}
#endif

#endif /* IRHotfixSDK_HFNativePatchRuntime_h */
