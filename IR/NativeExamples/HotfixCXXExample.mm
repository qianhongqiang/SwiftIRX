#include "HotfixNativeExamples.h"
#include "../../SDK/IRHotfixSDK/Runtime/HotfixNativeTarget.h"

class HFCalculator final {
public:
    IR_HOTFIX_TARGET
    long long multiply(long long value);
};

long long HFCalculator::multiply(long long value) {
    return value * 2;
}

extern "C" int64_t hotfix_example_cxx_multiply(int64_t value) {
    HFCalculator calculator;
    return static_cast<int64_t>(
        calculator.multiply(static_cast<long long>(value))
    );
}
