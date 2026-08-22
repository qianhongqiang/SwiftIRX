#ifndef HFDescriptor_h
#define HFDescriptor_h

#include <stdint.h>

#include "HFPatchFrame.h"

typedef uint32_t HFDescriptorFlags;
enum {
    HFDescriptorFlagNone = 0,
    HFDescriptorFlagHasReceiver = 1u << 0,
};

typedef struct HFDescriptor {
    uint32_t abiVersion;
    uint32_t structSize;
    uint64_t targetID;
    uint64_t signatureID;
    HFValueKind returnKind;
    uint32_t argumentCount;
    HFDescriptorFlags flags;
    uint32_t reserved;
    const char *symbol;
    const HFValueKind *argumentKinds;
} HFDescriptor;

#if defined(__cplusplus)
static_assert(sizeof(HFDescriptor) == 56, "HFDescriptor ABI layout changed");
#else
_Static_assert(sizeof(HFDescriptor) == 56, "HFDescriptor ABI layout changed");
#endif

#endif /* HFDescriptor_h */
