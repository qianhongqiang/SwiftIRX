#ifndef HFValue_h
#define HFValue_h

#include <stddef.h>
#include <stdint.h>

typedef uint32_t HFValueKind;
enum {
    HFValueKindInvalid = 0,
    HFValueKindSignedInteger = 1,
    HFValueKindBool = 2,
    HFValueKindVoid = 3,
    HFValueKindUnsignedInteger = 4,
    HFValueKindFloat32 = 5,
    HFValueKindFloat64 = 6,
    HFValueKindMemoryOffset = 7,
    HFValueKindHostHandle = 8,
    HFValueKindBytes = 9,
};

typedef uint32_t HFValueFlags;
enum {
    HFValueFlagNone = 0,
    /// The adapter guarantees this address remains valid through the complete
    /// patch invocation that contains the host call. It transfers no ownership.
    HFValueFlagBorrowedHostHandle = 1u << 0,
    /// Ownership of one Objective-C-compatible retain is transferred to the
    /// VM. The VM releases it with the Objective-C runtime. Never apply this
    /// flag to an unmanaged C or C++ pointer.
    HFValueFlagRetainedHostHandle = 1u << 1,
};

/// A null host handle is encoded as `HFValueKindHostHandle` with zero `bits`
/// and `HFValueFlagNone`; non-null handles must declare borrowed or retained
/// ownership.

typedef struct HFValue {
    HFValueKind kind;
    HFValueFlags flags;
    uint64_t bits;
    /// Borrowed aggregate storage. A result producer must keep this buffer
    /// alive until the immediate consumer has copied it after invocation.
    const void *bytes;
    uint64_t byteCount;
} HFValue;

static inline HFValue HFMakeValue(HFValueKind kind, uint64_t bits) {
    HFValue value = {kind, HFValueFlagNone, bits, NULL, 0};
    return value;
}

#if defined(__cplusplus)
static_assert(sizeof(HFValue) == 32, "HFValue ABI layout changed");
#else
_Static_assert(sizeof(HFValue) == 32, "HFValue ABI layout changed");
#endif

#endif /* HFValue_h */
