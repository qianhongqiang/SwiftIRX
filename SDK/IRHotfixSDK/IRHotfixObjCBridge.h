#ifndef IRHotfixObjCBridge_h
#define IRHotfixObjCBridge_h

#include <stddef.h>

#include "IRHotfixValue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IRHFObjCInvocationStatus {
    IRHFObjCInvocationStatusSuccess = 0,
    IRHFObjCInvocationStatusInvalidInput = 1,
    IRHFObjCInvocationStatusMethodNotFound = 2,
    IRHFObjCInvocationStatusArgumentCountMismatch = 3,
    IRHFObjCInvocationStatusUnsupportedArgumentType = 4,
    IRHFObjCInvocationStatusUnsupportedReturnType = 5,
    IRHFObjCInvocationStatusInvocationException = 6,
} IRHFObjCInvocationStatus;

typedef struct IRHFObjCInvocationResult {
    IRHFObjCInvocationStatus status;
    IRHFValue value;
} IRHFObjCInvocationResult;

/// Invokes an Objective-C instance or class method using its runtime method
/// signature. `receiver` is an unretained Objective-C object pointer.
///
/// Object results are returned at +1 ownership in `value.bits`. The caller must
/// either consume that ownership or call `IRHFObjCReleaseRetainedObject`.
IRHFObjCInvocationResult IRHFObjCInvoke(
    void *receiver,
    const char *selectorName,
    const IRHFValue *arguments,
    size_t argumentCount
);

void *IRHFObjCLookUpClass(const char *className);

void *IRHFObjCRegisterSelector(const char *selectorName);

void IRHFObjCReleaseRetainedObject(void *object);

const char *IRHFObjCInvocationStatusDescription(IRHFObjCInvocationStatus status);

#ifdef __cplusplus
}
#endif

#endif /* IRHotfixObjCBridge_h */
