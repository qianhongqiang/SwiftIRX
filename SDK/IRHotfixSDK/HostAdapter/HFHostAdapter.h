#ifndef IRHotfixSDK_HFHostAdapter_h
#define IRHotfixSDK_HFHostAdapter_h

#include <stddef.h>
#include <stdint.h>

#include "../ABI/HFPatchFrame.h"

#define HF_HOST_ADAPTER_ABI_VERSION 2u
#define HF_MAX_HOST_ARGUMENT_COUNT 16u

typedef uint32_t HFHostLanguage;
enum {
    HFHostLanguageObjectiveC = 1,
    HFHostLanguageC = 2,
    HFHostLanguageSwift = 3,
    HFHostLanguageCXX = 4,
};

typedef uint32_t HFHostCallKind;
enum {
    HFHostCallKindFunction = 1,
    HFHostCallKindInstanceMethod = 2,
    HFHostCallKindStaticMethod = 3,
    HFHostCallKindConstructor = 4,
    HFHostCallKindClassLookup = 5,
};

typedef uint32_t HFHostCallFlags;
enum {
    HFHostCallFlagNone = 0,
    HFHostCallFlagHasReceiver = 1u << 0,
    HFHostCallFlagMainThreadOnly = 1u << 1,
    /// Retained HostHandle results refer to table entries backed by
    /// Objective-C-compatible objects.
    HFHostCallFlagObjCCompatibleHandles = 1u << 2,
    /// The adapter does not mutate externally visible host state, including
    /// when it returns a failure. This permits safe native fallback.
    HFHostCallFlagNoSideEffects = 1u << 3,
};

/// Stable, language-neutral description of one callable host symbol.
/// String and argument-kind storage is borrowed only during registration or
/// invocation; the registry takes an owned copy when registering an adapter.
typedef struct HFHostCallDescriptor {
    uint32_t abiVersion;
    uint32_t structSize;
    uint64_t importID;
    uint64_t signatureID;
    HFHostLanguage language;
    HFHostCallKind callKind;
    HFValueKind returnKind;
    uint32_t argumentCount;
    HFHostCallFlags flags;
    uint32_t reserved;
    const char *owner;
    const char *name;
    const char *typeEncoding;
    const HFValueKind *argumentKinds;
} HFHostCallDescriptor;

/// The only ABI passed to C, Swift, and C++ host adapter entry points.
/// The adapter must not retain borrowed pointers from this frame.
typedef struct HFHostCallFrame {
    uint32_t abiVersion;
    uint32_t structSize;
    const HFHostCallDescriptor *descriptor;
    HFHandle receiver;
    const HFValue *arguments;
    uint32_t argumentCount;
    uint32_t reserved0;
    HFValue result;
    void *context;
    HFStatus status;
    uint32_t reserved1;
} HFHostCallFrame;

typedef HFStatus (*HFHostAdapterEntry)(HFHostCallFrame *frame);
typedef void (*HFHostAdapterContextRelease)(void *context);

/// Opaque identity for one concrete registration. The token prevents a stale
/// unregister from removing a newer adapter with the same symbol/signature.
typedef struct HFHostAdapterRegistration {
    uint64_t token;
    uint64_t importID;
    uint64_t signatureID;
} HFHostAdapterRegistration;

/// Opaque strong snapshot of one validated native adapter. A lease pins its
/// callable context across unregister/re-register until explicitly released.
typedef struct HFHostAdapterLease HFHostAdapterLease;

static inline HFHostAdapterRegistration HFInvalidHostAdapterRegistration(void) {
    HFHostAdapterRegistration registration = {0, 0, 0};
    return registration;
}

#ifdef __cplusplus
extern "C" {
#endif

uint64_t hf_host_call_id(const char *symbol);
uint64_t hf_host_call_signature_id(const HFHostCallDescriptor *descriptor);

HFStatus hf_host_adapter_register(
    const HFHostCallDescriptor *descriptor,
    HFHostAdapterEntry entry,
    void *context,
    HFHostAdapterContextRelease releaseContext,
    HFHostAdapterRegistration *registration
);

HFStatus hf_host_adapter_unregister(HFHostAdapterRegistration registration);

/// Validates that the descriptor is structurally sound and, for native calls,
/// currently resolves to a compatible registered adapter without invoking it.
HFStatus hf_host_adapter_validate(const HFHostCallDescriptor *descriptor);

HFStatus hf_host_adapter_acquire(
    const HFHostCallDescriptor *descriptor,
    HFHostAdapterLease **lease
);
void hf_host_adapter_release(HFHostAdapterLease *lease);
HFStatus hf_host_adapter_invoke_leased(
    HFHostAdapterLease *lease,
    HFHandle receiver,
    const HFValue *arguments,
    uint32_t argumentCount,
    HFValue *result
);
HFHostCallFlags hf_host_adapter_lease_flags(const HFHostAdapterLease *lease);

/// Resolves and invokes Objective-C built-ins or a registered C/Swift/C++
/// gateway. Descriptor and frame validation happens before adapter code runs.
HFStatus hf_host_adapter_invoke(
    const HFHostCallDescriptor *descriptor,
    HFHandle receiver,
    const HFValue *arguments,
    uint32_t argumentCount,
    HFValue *result
);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(HFHostCallDescriptor) == 80,
              "HFHostCallDescriptor ABI layout changed");
static_assert(sizeof(HFHostCallFrame) == 96,
              "HFHostCallFrame ABI layout changed");
static_assert(sizeof(HFHostAdapterRegistration) == 24,
              "HFHostAdapterRegistration ABI layout changed");
#else
_Static_assert(sizeof(HFHostCallDescriptor) == 80,
               "HFHostCallDescriptor ABI layout changed");
_Static_assert(sizeof(HFHostCallFrame) == 96,
               "HFHostCallFrame ABI layout changed");
_Static_assert(sizeof(HFHostAdapterRegistration) == 24,
               "HFHostAdapterRegistration ABI layout changed");
#endif

#endif /* IRHotfixSDK_HFHostAdapter_h */
