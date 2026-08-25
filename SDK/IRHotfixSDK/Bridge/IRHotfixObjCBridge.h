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

/// Allocates an Objective-C instance and invokes its initializer. The returned
/// object follows the same +1 ownership contract as IRHFObjCInvoke results.
IRHFObjCInvocationResult IRHFObjCConstruct(
    void *classObject,
    const char *initializerName,
    const IRHFValue *arguments,
    size_t argumentCount
);

/// Creates an NSString from UTF-8 bytes and returns it at +1 ownership.
void *IRHFObjCCreateStringUTF8(const void *bytes, size_t byteCount);

/// Concatenates two NSString-compatible objects and returns +1 ownership.
void *IRHFObjCCreateConcatenatedString(void *left, void *right);

/// Returns nonzero when `object` is NSString-compatible.
int IRHFObjCIsString(void *object);

/// Returns nonzero when the caller is running on the process main thread.
int IRHFObjCIsMainThread(void);

void *IRHFObjCLookUpClass(const char *className);

void *IRHFObjCRegisterSelector(const char *selectorName);

void IRHFObjCReleaseRetainedObject(void *object);

const char *IRHFObjCInvocationStatusDescription(IRHFObjCInvocationStatus status);

#ifdef __cplusplus
}
#endif

#endif /* IRHotfixObjCBridge_h */
