#ifndef HFValue_h
#define HFValue_h

#include <stddef.h>
#include <stdint.h>

#include "HFHandle.h"

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
    /// The encoded table handle remains registered elsewhere for the duration
    /// of the host call. It transfers no handle-table ownership.
    HFValueFlagBorrowedHostHandle = 1u << 0,
    /// Ownership of the encoded handle-table entry is transferred to the VM.
    /// The VM releases the handle entry instead of releasing a raw pointer.
    HFValueFlagRetainedHostHandle = 1u << 1,
};

/// Host handles preserve the fixed 32-byte HFValue ABI. `bits` stores the
/// opaque handle token and `byteCount` stores generation/kind/flags. `bytes`
/// is always null. The payload must be decoded with HFValueGetHostHandle.

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

static inline HFValue HFValueFromHostHandle(HFHandle handle,
                                             HFValueFlags ownership) {
    HFValue value = HFMakeValue(HFValueKindHostHandle, handle.token);
    value.flags = handle.token == 0 ? HFValueFlagNone : ownership;
    value.byteCount = ((uint64_t)handle.generation << 32) |
                      ((uint64_t)handle.kind << 16) |
                      (uint64_t)handle.flags;
    return value;
}

static inline int HFValueGetHostHandle(const HFValue *value,
                                       HFHandle *handle) {
    if (value == NULL || handle == NULL ||
        value->kind != HFValueKindHostHandle || value->bytes != NULL) {
        return 0;
    }
    if (value->bits == 0) {
        if (value->flags != HFValueFlagNone || value->byteCount != 0) {
            return 0;
        }
        *handle = HFInvalidHandle();
        return 1;
    }
    if (value->flags != HFValueFlagBorrowedHostHandle &&
        value->flags != HFValueFlagRetainedHostHandle) {
        return 0;
    }
    handle->token = value->bits;
    handle->generation = (uint32_t)(value->byteCount >> 32);
    handle->kind = (HFHandleKind)((value->byteCount >> 16) & 0xffffu);
    handle->flags = (HFHandleFlags)(value->byteCount & 0xffffu);
    return handle->generation != 0 && handle->kind != HFHandleKindInvalid;
}

#if defined(__cplusplus)
static_assert(sizeof(HFValue) == 32, "HFValue ABI layout changed");
#else
_Static_assert(sizeof(HFValue) == 32, "HFValue ABI layout changed");
#endif

#endif /* HFValue_h */
