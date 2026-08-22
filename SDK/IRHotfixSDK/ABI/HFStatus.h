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
};

#endif /* HFStatus_h */
