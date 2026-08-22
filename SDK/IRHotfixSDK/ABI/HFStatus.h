#ifndef HFStatus_h
#define HFStatus_h

#include <stdint.h>

typedef int32_t HFStatus;

enum {
    HFStatusApplied = 0,
    HFStatusNoPatch = 1,
    HFStatusInvalidFrame = 2,
    HFStatusABIVersionMismatch = 3,
    HFStatusSignatureMismatch = 4,
    HFStatusInvalidArguments = 5,
    HFStatusExecutionFailed = 6,
    HFStatusInvalidResult = 7,
    HFStatusHostAdapterNotFound = 8,
    HFStatusHostAdapterConflict = 9,
    /// Host execution may already have produced externally visible effects.
    /// A trampoline must never fall back to the original implementation.
    HFStatusExecutionCommitted = 10,
};

#endif /* HFStatus_h */
