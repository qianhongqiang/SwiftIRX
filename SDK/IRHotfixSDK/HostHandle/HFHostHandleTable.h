#ifndef IRHotfixSDK_HFHostHandleTable_h
#define IRHotfixSDK_HFHostHandleTable_h

#include "../ABI/HFHandle.h"
#include "../ABI/HFStatus.h"

typedef uint16_t HFHostHandleOwnership;
enum {
    HFHostHandleOwnershipBorrowed = HFHandleFlagBorrowed,
    HFHostHandleOwnershipStrong = HFHandleFlagRetained,
    HFHostHandleOwnershipWeak = HFHandleFlagWeak,
};

/// A resolve lease validates token + generation and pins Objective-C/Swift
/// objects for the complete host invocation that consumes the raw address.
typedef struct HFHostHandleLease HFHostHandleLease;

#ifdef __cplusplus
extern "C" {
#endif

HFStatus hf_host_handle_register(
    void *value,
    HFHandleKind kind,
    HFHostHandleOwnership ownership,
    HFHandle *handle
);

/// Convenience lifecycle used by synchronous patch trampolines. A borrowed
/// entry is valid until the matching scope-end call.
HFStatus hf_host_handle_scope_begin(
    void *value,
    HFHandleKind kind,
    HFHandle *handle
);
HFStatus hf_host_handle_scope_end(HFHandle handle);
HFStatus hf_host_handle_scope_end_ref(const HFHandle *handle);

HFStatus hf_host_handle_validate(HFHandle handle);
HFStatus hf_host_handle_resolve(
    HFHandle handle,
    HFHostHandleLease **lease,
    void **value
);
void hf_host_handle_lease_release(HFHostHandleLease *lease);

/// Creates a new strong table entry from any live object/class/block handle.
HFStatus hf_host_handle_retain(HFHandle handle, HFHandle *retainedHandle);

/// Invalidates this exact token + generation. Existing resolve leases keep a
/// resolved object pinned, while subsequent resolves report StaleHandle.
HFStatus hf_host_handle_release(HFHandle handle);

uint64_t hf_host_handle_live_count(void);

#ifdef __cplusplus
}
#endif

#endif /* IRHotfixSDK_HFHostHandleTable_h */
