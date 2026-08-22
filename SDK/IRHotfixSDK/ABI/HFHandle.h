#ifndef HFHandle_h
#define HFHandle_h

#include <stdint.h>

typedef uint16_t HFHandleKind;
enum {
    HFHandleKindInvalid = 0,
    HFHandleKindObject = 1,
    HFHandleKindClass = 2,
    HFHandleKindSelector = 3,
    HFHandleKindBlock = 4,
    HFHandleKindNativeSymbol = 5,
};

typedef uint16_t HFHandleFlags;
enum {
    HFHandleFlagNone = 0,
    HFHandleFlagBorrowed = 1u << 0,
    HFHandleFlagRetained = 1u << 1,
    /// Phase-1 compatibility: `token` is a synchronous borrowed host address.
    /// The VM must still treat it as opaque and resolve it through the gateway.
    HFHandleFlagBorrowedAddress = 1u << 15,
};

typedef struct HFHandle {
    uint64_t token;
    uint32_t generation;
    HFHandleKind kind;
    HFHandleFlags flags;
} HFHandle;

static inline HFHandle HFInvalidHandle(void) {
    HFHandle handle = {0, 0, HFHandleKindInvalid, HFHandleFlagNone};
    return handle;
}

#if defined(__cplusplus)
static_assert(sizeof(HFHandle) == 16, "HFHandle ABI layout changed");
#else
_Static_assert(sizeof(HFHandle) == 16, "HFHandle ABI layout changed");
#endif

#endif /* HFHandle_h */
