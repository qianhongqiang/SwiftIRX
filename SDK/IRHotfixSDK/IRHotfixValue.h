#ifndef IRHotfixValue_h
#define IRHotfixValue_h

#include <stddef.h>
#include <stdint.h>

/// Adapter-neutral value ABI shared by the interpreter and host-language
/// invocation adapters. Object and pointer payloads are carried in `bits`;
/// aggregate payloads borrow `bytes` for the duration of a synchronous call.
typedef enum IRHFValueKind {
    IRHFValueKindInvalid = 0,
    IRHFValueKindSignedInteger = 1,
    IRHFValueKindUnsignedInteger = 2,
    IRHFValueKindBool = 3,
    IRHFValueKindFloat32 = 4,
    IRHFValueKindFloat64 = 5,
    IRHFValueKindPointer = 6,
    IRHFValueKindObject = 7,
    IRHFValueKindBytes = 8,
    IRHFValueKindVoid = 9,
} IRHFValueKind;

typedef struct IRHFValue {
    IRHFValueKind kind;
    uint64_t bits;
    const void *bytes;
    size_t byteCount;
} IRHFValue;

static inline IRHFValue IRHFMakeValue(IRHFValueKind kind, uint64_t bits) {
    IRHFValue value = {kind, bits, NULL, 0};
    return value;
}

#endif /* IRHotfixValue_h */
