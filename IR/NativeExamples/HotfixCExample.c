#include "HotfixNativeExamples.h"
#include "../../SDK/IRHotfixSDK/Runtime/HotfixNativeTarget.h"

IR_HOTFIX_TARGET
int64_t hotfix_example_c_add(int64_t value) {
    return value + 2;
}
