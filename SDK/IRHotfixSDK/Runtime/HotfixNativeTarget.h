#ifndef IRHotfixSDK_HotfixNativeTarget_h
#define IRHotfixSDK_HotfixNativeTarget_h

#if !defined(__clang__)
#error "IR_HOTFIX_TARGET requires Clang"
#endif

/// Marks a C function or C++ method as a binary-HFIR hotfix target. The native
/// compiler wrapper validates the source ABI, emits the runtime trampoline and
/// writes the per-object Target Manifest sidecar.
#define IR_HOTFIX_TARGET                                                     \
  __attribute__((annotate("ir_hotfix_target"), noinline, used,              \
                 visibility("default")))

#endif /* IRHotfixSDK_HotfixNativeTarget_h */
