#include "../../../../SDK/IRHotfixSDK/Runtime/HotfixNativeTarget.h"

class HFCalculator final {
public:
  IR_HOTFIX_TARGET
  long long multiply(long long value);
};

long long HFCalculator::multiply(long long value) {
  return value * 5;
}
