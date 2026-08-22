#ifndef IRHotfixSDK_HFIRRuntime_h
#define IRHotfixSDK_HFIRRuntime_h

#include <stddef.h>
#include <stdint.h>

#include "../ABI/HFPatchFrame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HFIRPatchHandle {
    uint64_t token;
    uint64_t targetID;
    uint64_t signatureID;
} HFIRPatchHandle;

HFStatus hf_hfir_vm_install(
    const void *bytes,
    size_t byteCount,
    HFIRPatchHandle *handle
);

HFStatus hf_hfir_vm_activate(HFIRPatchHandle handle);
HFStatus hf_hfir_vm_deactivate(HFIRPatchHandle handle);
HFStatus hf_hfir_vm_uninstall(HFIRPatchHandle handle);

/// Executes only an active HFIR patch. Returns HFStatusNoPatch when the target
/// is not owned by this VM so the compatibility text engine can be attempted.
HFStatus hf_hfir_vm_invoke(HFPatchFrame *frame);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(HFIRPatchHandle) == 24,
              "HFIRPatchHandle ABI layout changed");
#else
_Static_assert(sizeof(HFIRPatchHandle) == 24,
               "HFIRPatchHandle ABI layout changed");
#endif

#endif /* IRHotfixSDK_HFIRRuntime_h */
