#include "../../../../SDK/IRHotfixSDK/Runtime/HotfixNativeTarget.h"

class InvalidCalculator {
public:
  IR_HOTFIX_TARGET
  virtual long long multiply(long long value) {
    return value * 5;
  }
};
