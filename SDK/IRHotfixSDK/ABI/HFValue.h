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
};

typedef struct HFValue {
    HFValueKind kind;
    HFValueFlags flags;
    uint64_t bits;
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
