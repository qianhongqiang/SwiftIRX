#import "IRHotfixObjCBridge.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

@interface HFObjCInvocationDescriptor : NSObject

@property(nonatomic, readonly) NSMethodSignature *signature;
@property(nonatomic, copy, readonly) NSString *typeEncoding;

- (instancetype)initWithSignature:(NSMethodSignature *)signature
                      typeEncoding:(NSString *)typeEncoding;

@end

@implementation HFObjCInvocationDescriptor

- (instancetype)initWithSignature:(NSMethodSignature *)signature
                      typeEncoding:(NSString *)typeEncoding {
    self = [super init];
    if (self) {
        _signature = signature;
        _typeEncoding = [typeEncoding copy];
    }
    return self;
}

@end

@interface HFObjCInvoker : NSObject

+ (instancetype)sharedInvoker;
- (nullable HFObjCInvocationDescriptor *)descriptorForReceiver:(id)receiver
                                                       selector:(SEL)selector;

@end

@implementation HFObjCInvoker {
    NSCache<NSString *, HFObjCInvocationDescriptor *> *_descriptorCache;
}

+ (instancetype)sharedInvoker {
    static HFObjCInvoker *invoker;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        invoker = [[HFObjCInvoker alloc] init];
    });
    return invoker;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _descriptorCache = [[NSCache alloc] init];
        _descriptorCache.countLimit = 512;
    }
    return self;
}

- (nullable HFObjCInvocationDescriptor *)descriptorForReceiver:(id)receiver
                                                       selector:(SEL)selector {
    Class dispatchClass = object_getClass(receiver);
    Method method = class_getInstanceMethod(dispatchClass, selector);
    const char *runtimeEncoding = method == nullptr ? nullptr : method_getTypeEncoding(method);

    if (runtimeEncoding != nullptr) {
        NSString *typeEncoding = [NSString stringWithUTF8String:runtimeEncoding];
        NSString *key = [NSString stringWithFormat:@"%p|%p|%@",
                                                   (void *)dispatchClass,
                                                   (void *)selector,
                                                   typeEncoding];
        HFObjCInvocationDescriptor *cached = [_descriptorCache objectForKey:key];
        if (cached != nil) {
            return cached;
        }

        NSMethodSignature *signature = [receiver methodSignatureForSelector:selector];
        if (signature == nil) {
            return nil;
        }
        HFObjCInvocationDescriptor *descriptor =
            [[HFObjCInvocationDescriptor alloc] initWithSignature:signature
                                                     typeEncoding:typeEncoding];
        [_descriptorCache setObject:descriptor forKey:key];
        return descriptor;
    }

    NSMethodSignature *signature = [receiver methodSignatureForSelector:selector];
    if (signature == nil) {
        return nil;
    }

    NSMutableString *canonicalEncoding = [NSMutableString string];
    [canonicalEncoding appendFormat:@"%s", signature.methodReturnType];
    for (NSUInteger index = 0; index < signature.numberOfArguments; ++index) {
        [canonicalEncoding appendFormat:@"%s", [signature getArgumentTypeAtIndex:index]];
    }

    NSString *key = [NSString stringWithFormat:@"%p|%p|%@",
                                               (void *)dispatchClass,
                                               (void *)selector,
                                               canonicalEncoding];
    HFObjCInvocationDescriptor *cached = [_descriptorCache objectForKey:key];
    if (cached != nil) {
        return cached;
    }

    HFObjCInvocationDescriptor *descriptor =
        [[HFObjCInvocationDescriptor alloc] initWithSignature:signature
                                                 typeEncoding:canonicalEncoding];
    [_descriptorCache setObject:descriptor forKey:key];
    return descriptor;
}

@end

namespace {

const char *skipTypeQualifiers(const char *type) {
    if (type == nullptr) {
        return nullptr;
    }
    while (*type == 'r' || *type == 'n' || *type == 'N' || *type == 'o' ||
           *type == 'O' || *type == 'R' || *type == 'V') {
        ++type;
    }
    return type;
}

IRHFValue emptyValue(IRHFValueKind kind = IRHFValueKindInvalid) {
    IRHFValue value = {};
    value.kind = kind;
    return value;
}

IRHFObjCInvocationResult resultWithStatus(IRHFObjCInvocationStatus status) {
    IRHFObjCInvocationResult result = {};
    result.status = status;
    result.value = emptyValue();
    return result;
}

bool methodFamilyMatches(const char *selectorName, const char *family) {
    const size_t familyLength = std::strlen(family);
    if (std::strncmp(selectorName, family, familyLength) != 0) {
        return false;
    }
    const unsigned char next = static_cast<unsigned char>(selectorName[familyLength]);
    return next == '\0' || !std::islower(next);
}

bool returnsRetainedObject(const char *selectorName) {
    return methodFamilyMatches(selectorName, "alloc") ||
           methodFamilyMatches(selectorName, "new") ||
           methodFamilyMatches(selectorName, "copy") ||
           methodFamilyMatches(selectorName, "mutableCopy") ||
           methodFamilyMatches(selectorName, "init");
}

bool marshalInteger(uint64_t bits, void *destination, size_t size) {
    if (size == 0 || size > sizeof(bits)) {
        return false;
    }
    std::memcpy(destination, &bits, size);
    return true;
}

uint64_t signExtendInteger(uint64_t bits, size_t size) {
    if (size >= sizeof(bits)) {
        return bits;
    }
    const unsigned bitCount = static_cast<unsigned>(size * 8);
    const uint64_t signBit = uint64_t{1} << (bitCount - 1);
    const uint64_t valueMask = (uint64_t{1} << bitCount) - 1;
    bits &= valueMask;
    return (bits ^ signBit) - signBit;
}

bool marshalArgument(
    const IRHFValue &value,
    const char *rawType,
    std::vector<uint8_t> &storage
) {
    const char *type = skipTypeQualifiers(rawType);
    if (type == nullptr || *type == '\0') {
        return false;
    }

    NSUInteger size = 0;
    NSUInteger alignment = 0;
    NSGetSizeAndAlignment(type, &size, &alignment);
    if (size == 0) {
        return false;
    }
    storage.assign(size, 0);

    switch (*type) {
        case '@':
        case '#': {
            if (value.kind != IRHFValueKindObject &&
                value.kind != IRHFValueKindPointer) {
                return false;
            }
            void *pointer = reinterpret_cast<void *>(static_cast<uintptr_t>(value.bits));
            std::memcpy(storage.data(), &pointer, sizeof(pointer));
            return true;
        }
        case ':':
        case '^':
        case '*':
        case '?': {
            if (value.kind != IRHFValueKindPointer &&
                value.kind != IRHFValueKindObject) {
                return false;
            }
            void *pointer = reinterpret_cast<void *>(static_cast<uintptr_t>(value.bits));
            std::memcpy(storage.data(), &pointer, sizeof(pointer));
            return true;
        }
        case 'B':
        case 'c':
        case 'C':
        case 's':
        case 'S':
        case 'i':
        case 'I':
        case 'l':
        case 'L':
        case 'q':
        case 'Q': {
            if (value.kind != IRHFValueKindSignedInteger &&
                value.kind != IRHFValueKindUnsignedInteger &&
                value.kind != IRHFValueKindBool) {
                return false;
            }
            return marshalInteger(value.bits, storage.data(), size);
        }
        case 'f': {
            if (value.kind != IRHFValueKindFloat32) {
                return false;
            }
            const uint32_t bits = static_cast<uint32_t>(value.bits);
            std::memcpy(storage.data(), &bits, sizeof(bits));
            return true;
        }
        case 'd': {
            if (value.kind != IRHFValueKindFloat64) {
                return false;
            }
            std::memcpy(storage.data(), &value.bits, sizeof(value.bits));
            return true;
        }
        case '{':
        case '[':
        case '(': {
            if (value.kind != IRHFValueKindBytes || value.bytes == nullptr ||
                value.byteCount != size) {
                return false;
            }
            std::memcpy(storage.data(), value.bytes, size);
            return true;
        }
        default:
            return false;
    }
}

IRHFObjCInvocationResult readReturnValue(
    NSInvocation *invocation,
    NSMethodSignature *signature,
    const char *selectorName
) {
    const char *type = skipTypeQualifiers(signature.methodReturnType);
    if (type == nullptr || *type == '\0') {
        return resultWithStatus(IRHFObjCInvocationStatusUnsupportedReturnType);
    }

    IRHFObjCInvocationResult result = resultWithStatus(IRHFObjCInvocationStatusSuccess);
    if (*type == 'v') {
        result.value = emptyValue(IRHFValueKindVoid);
        return result;
    }

    const NSUInteger length = signature.methodReturnLength;
    if (*type == '@' || *type == '#') {
        void *object = nullptr;
        [invocation getReturnValue:&object];
        if (object != nullptr && !returnsRetainedObject(selectorName)) {
            CFRetain(static_cast<CFTypeRef>(object));
        }
        result.value = emptyValue(IRHFValueKindObject);
        result.value.bits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object));
        return result;
    }

    if (*type == ':' || *type == '^' || *type == '*' || *type == '?') {
        void *pointer = nullptr;
        [invocation getReturnValue:&pointer];
        result.value = emptyValue(IRHFValueKindPointer);
        result.value.bits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
        return result;
    }

    if (length == 0 || length > sizeof(uint64_t)) {
        return resultWithStatus(IRHFObjCInvocationStatusUnsupportedReturnType);
    }

    uint64_t bits = 0;
    [invocation getReturnValue:&bits];
    switch (*type) {
        case 'B':
            result.value = emptyValue(IRHFValueKindBool);
            break;
        case 'c':
        case 's':
        case 'i':
        case 'l':
        case 'q':
            result.value = emptyValue(IRHFValueKindSignedInteger);
            bits = signExtendInteger(bits, length);
            break;
        case 'C':
        case 'S':
        case 'I':
        case 'L':
        case 'Q':
            result.value = emptyValue(IRHFValueKindUnsignedInteger);
            break;
        case 'f':
            result.value = emptyValue(IRHFValueKindFloat32);
            break;
        case 'd':
            result.value = emptyValue(IRHFValueKindFloat64);
            break;
        default:
            return resultWithStatus(IRHFObjCInvocationStatusUnsupportedReturnType);
    }
    result.value.bits = bits;
    return result;
}

} // namespace

IRHFObjCInvocationResult IRHFObjCInvoke(
    void *receiver,
    const char *selectorName,
    const IRHFValue *arguments,
    size_t argumentCount
) {
    if (receiver == nullptr || selectorName == nullptr ||
        (argumentCount != 0 && arguments == nullptr)) {
        return resultWithStatus(IRHFObjCInvocationStatusInvalidInput);
    }

    id object = (__bridge id)receiver;
    SEL selector = sel_registerName(selectorName);
    HFObjCInvocationDescriptor *descriptor =
        [[HFObjCInvoker sharedInvoker] descriptorForReceiver:object selector:selector];
    if (descriptor == nil) {
        return resultWithStatus(IRHFObjCInvocationStatusMethodNotFound);
    }
    NSMethodSignature *signature = descriptor.signature;
    if (signature.numberOfArguments != argumentCount + 2) {
        return resultWithStatus(IRHFObjCInvocationStatusArgumentCountMismatch);
    }

    NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
    invocation.target = object;
    invocation.selector = selector;

    std::vector<std::vector<uint8_t>> argumentStorage(argumentCount);
    for (size_t index = 0; index < argumentCount; ++index) {
        const char *type = [signature getArgumentTypeAtIndex:index + 2];
        if (!marshalArgument(arguments[index], type, argumentStorage[index])) {
            return resultWithStatus(IRHFObjCInvocationStatusUnsupportedArgumentType);
        }
        [invocation setArgument:argumentStorage[index].data() atIndex:index + 2];
    }
    [invocation retainArguments];

    @try {
        [invocation invoke];
    } @catch (__unused NSException *exception) {
        return resultWithStatus(IRHFObjCInvocationStatusInvocationException);
    }

    return readReturnValue(invocation, signature, selectorName);
}

IRHFObjCInvocationResult IRHFObjCConstruct(
    void *classObject,
    const char *initializerName,
    const IRHFValue *arguments,
    size_t argumentCount
) {
    if (classObject == nullptr || initializerName == nullptr) {
        return resultWithStatus(IRHFObjCInvocationStatusInvalidInput);
    }
    Class objectClass = (__bridge Class)classObject;
    id allocated = [objectClass alloc];
    if (allocated == nil) {
        return resultWithStatus(IRHFObjCInvocationStatusInvocationException);
    }
    IRHFObjCInvocationResult result =
        IRHFObjCInvoke((__bridge void *)allocated, initializerName,
                       arguments, argumentCount);
    if (result.status == IRHFObjCInvocationStatusSuccess &&
        result.value.kind == IRHFValueKindObject && result.value.bits != 0) {
        void *returned = reinterpret_cast<void *>(
            static_cast<uintptr_t>(result.value.bits));
        if (returned == (__bridge void *)allocated) {
            // The local strong reference owns this alloc/init result. Transfer
            // one independent retain before ARC releases the local reference.
            CFRetain(reinterpret_cast<CFTypeRef>(returned));
        }
    }
    return result;
}

void *IRHFObjCCreateStringUTF8(const void *bytes, size_t byteCount) {
    if (byteCount != 0 && bytes == nullptr) {
        return nullptr;
    }
    NSString *string = [[NSString alloc]
        initWithBytes:bytes
              length:byteCount
            encoding:NSUTF8StringEncoding];
    return (__bridge_retained void *)string;
}

void *IRHFObjCCreateConcatenatedString(void *left, void *right) {
    if (left == nullptr || right == nullptr) {
        return nullptr;
    }
    id leftObject = (__bridge id)left;
    id rightObject = (__bridge id)right;
    if (![leftObject isKindOfClass:NSString.class] ||
        ![rightObject isKindOfClass:NSString.class]) {
        return nullptr;
    }
    NSString *result = [(NSString *)leftObject
        stringByAppendingString:(NSString *)rightObject];
    return (__bridge_retained void *)result;
}

int IRHFObjCIsString(void *object) {
    if (object == nullptr) {
        return 0;
    }
    return [(__bridge id)object isKindOfClass:NSString.class] ? 1 : 0;
}

int IRHFObjCIsMainThread(void) {
    return [NSThread isMainThread] ? 1 : 0;
}

void *IRHFObjCLookUpClass(const char *className) {
    if (className == nullptr) {
        return nullptr;
    }
    return (__bridge void *)objc_lookUpClass(className);
}

void *IRHFObjCRegisterSelector(const char *selectorName) {
    if (selectorName == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void *>(sel_registerName(selectorName));
}

void IRHFObjCReleaseRetainedObject(void *object) {
    if (object != nullptr) {
        CFRelease(static_cast<CFTypeRef>(object));
    }
}

const char *IRHFObjCInvocationStatusDescription(IRHFObjCInvocationStatus status) {
    switch (status) {
        case IRHFObjCInvocationStatusSuccess:
            return "success";
        case IRHFObjCInvocationStatusInvalidInput:
            return "invalid input";
        case IRHFObjCInvocationStatusMethodNotFound:
            return "method not found";
        case IRHFObjCInvocationStatusArgumentCountMismatch:
            return "argument count mismatch";
        case IRHFObjCInvocationStatusUnsupportedArgumentType:
            return "unsupported argument type";
        case IRHFObjCInvocationStatusUnsupportedReturnType:
            return "unsupported return type";
        case IRHFObjCInvocationStatusInvocationException:
            return "Objective-C invocation raised an exception";
    }
    return "unknown status";
}
