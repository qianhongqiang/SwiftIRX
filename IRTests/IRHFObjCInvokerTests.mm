#import <UIKit/UIKit.h>
#import <XCTest/XCTest.h>

#import "../SDK/IRHotfixSDK/Bridge/IRHotfixObjCBridge.h"

#include <cstring>

@interface IRHFObjCInvokerFixture : NSObject

@property(nonatomic) int8_t signed8Value;
@property(nonatomic) uint32_t unsigned32Value;
@property(nonatomic) float float32Value;
@property(nonatomic) double float64Value;
@property(nonatomic) void *pointerValue;
@property(nonatomic) CGRect rectValue;

- (BOOL)acceptSelector:(SEL)selector;
- (void)raiseTestException;

@end

@implementation IRHFObjCInvokerFixture

- (BOOL)acceptSelector:(SEL)selector {
    return selector == @selector(description);
}

- (void)raiseTestException {
    [NSException raise:NSInternalInconsistencyException format:@"test exception"];
}

@end

@interface IRHFObjCInvokerTests : XCTestCase
@end

@implementation IRHFObjCInvokerTests

- (void)testDescriptorDrivenScalarPointerAndAggregateMarshalling {
    IRHFObjCInvokerFixture *fixture = [[IRHFObjCInvokerFixture alloc] init];
    void *receiver = (__bridge void *)fixture;

    IRHFValue signedArgument = IRHFMakeValue(IRHFValueKindSignedInteger, UINT64_MAX - 6);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setSigned8Value:", &signedArgument, 1).status,
                   IRHFObjCInvocationStatusSuccess);
    IRHFObjCInvocationResult signedResult = IRHFObjCInvoke(receiver, "signed8Value", nullptr, 0);
    XCTAssertEqual(signedResult.status, IRHFObjCInvocationStatusSuccess);
    XCTAssertEqual(signedResult.value.kind, IRHFValueKindSignedInteger);
    XCTAssertEqual(signedResult.value.bits, UINT64_MAX - 6);

    IRHFValue unsignedArgument = IRHFMakeValue(IRHFValueKindUnsignedInteger, 0xFEDCBA98u);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setUnsigned32Value:", &unsignedArgument, 1).status,
                   IRHFObjCInvocationStatusSuccess);
    IRHFObjCInvocationResult unsignedResult = IRHFObjCInvoke(receiver, "unsigned32Value", nullptr, 0);
    XCTAssertEqual(unsignedResult.status, IRHFObjCInvocationStatusSuccess);
    XCTAssertEqual(unsignedResult.value.kind, IRHFValueKindUnsignedInteger);
    XCTAssertEqual(unsignedResult.value.bits, 0xFEDCBA98u);

    const float floatValue = 3.25f;
    uint32_t floatBits = 0;
    std::memcpy(&floatBits, &floatValue, sizeof(floatBits));
    IRHFValue floatArgument = IRHFMakeValue(IRHFValueKindFloat32, floatBits);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setFloat32Value:", &floatArgument, 1).status,
                   IRHFObjCInvocationStatusSuccess);
    IRHFObjCInvocationResult floatResult = IRHFObjCInvoke(receiver, "float32Value", nullptr, 0);
    XCTAssertEqual(floatResult.status, IRHFObjCInvocationStatusSuccess);
    XCTAssertEqual(floatResult.value.kind, IRHFValueKindFloat32);
    XCTAssertEqual((uint32_t)floatResult.value.bits, floatBits);

    const double doubleValue = -42.5;
    uint64_t doubleBits = 0;
    std::memcpy(&doubleBits, &doubleValue, sizeof(doubleBits));
    IRHFValue doubleArgument = IRHFMakeValue(IRHFValueKindFloat64, doubleBits);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setFloat64Value:", &doubleArgument, 1).status,
                   IRHFObjCInvocationStatusSuccess);
    IRHFObjCInvocationResult doubleResult = IRHFObjCInvoke(receiver, "float64Value", nullptr, 0);
    XCTAssertEqual(doubleResult.status, IRHFObjCInvocationStatusSuccess);
    XCTAssertEqual(doubleResult.value.kind, IRHFValueKindFloat64);
    XCTAssertEqual(doubleResult.value.bits, doubleBits);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "float64Value", nullptr, 0).value.bits, doubleBits);

    NSInteger storage = 42;
    IRHFValue pointerArgument = IRHFMakeValue(
        IRHFValueKindPointer,
        (uint64_t)(uintptr_t)&storage
    );
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setPointerValue:", &pointerArgument, 1).status,
                   IRHFObjCInvocationStatusSuccess);
    IRHFObjCInvocationResult pointerResult = IRHFObjCInvoke(receiver, "pointerValue", nullptr, 0);
    XCTAssertEqual(pointerResult.status, IRHFObjCInvocationStatusSuccess);
    XCTAssertEqual(pointerResult.value.kind, IRHFValueKindPointer);
    XCTAssertEqual(pointerResult.value.bits, (uint64_t)(uintptr_t)&storage);

    IRHFValue selectorArgument = IRHFMakeValue(
        IRHFValueKindPointer,
        (uint64_t)(uintptr_t)IRHFObjCRegisterSelector("description")
    );
    IRHFObjCInvocationResult selectorResult =
        IRHFObjCInvoke(receiver, "acceptSelector:", &selectorArgument, 1);
    XCTAssertEqual(selectorResult.status, IRHFObjCInvocationStatusSuccess);
    XCTAssertEqual(selectorResult.value.kind, IRHFValueKindBool);
    XCTAssertEqual(selectorResult.value.bits, 1u);

    CGRect rect = CGRectMake(1, 2, 30, 40);
    IRHFValue rectArgument = {};
    rectArgument.kind = IRHFValueKindBytes;
    rectArgument.bytes = &rect;
    rectArgument.byteCount = sizeof(rect);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setRectValue:", &rectArgument, 1).status,
                   IRHFObjCInvocationStatusSuccess);
    XCTAssertTrue(CGRectEqualToRect(fixture.rectValue, rect));
    XCTAssertEqual(IRHFObjCInvoke(receiver, "rectValue", nullptr, 0).status,
                   IRHFObjCInvocationStatusUnsupportedReturnType);
}

- (void)testDescriptorFailuresAreExplicit {
    IRHFObjCInvokerFixture *fixture = [[IRHFObjCInvokerFixture alloc] init];
    void *receiver = (__bridge void *)fixture;

    XCTAssertEqual(IRHFObjCInvoke(receiver, "missingSelector", nullptr, 0).status,
                   IRHFObjCInvocationStatusMethodNotFound);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "setSigned8Value:", nullptr, 0).status,
                   IRHFObjCInvocationStatusArgumentCountMismatch);
    XCTAssertEqual(IRHFObjCInvoke(receiver, "raiseTestException", nullptr, 0).status,
                   IRHFObjCInvocationStatusInvocationException);
    XCTAssertEqual(std::strcmp(
                       IRHFObjCInvocationStatusDescription((IRHFObjCInvocationStatus)999),
                       "unknown status"
                   ),
                   0);
}

@end
