#include "../../SDK/IRHotfixSDK/Runtime/HotfixNativeTarget.h"

__attribute__((noinline))
static long long patchedMultiply(long long value) {
    return value * 5;
}

class HFCalculator final {
public:
    IR_HOTFIX_TARGET
    long long multiply(long long value);
};

long long HFCalculator::multiply(long long value) {
    return patchedMultiply(value);
}
