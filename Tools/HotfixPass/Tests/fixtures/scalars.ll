; RUN: opt -load-pass-plugin %plugin -passes=hotfix-instrument -S %s | FileCheck %s

; CHECK-LABEL: define swiftcc i64 @integerTarget(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS:[0-9]+]] {
; CHECK: %[[INT_KINDS:[^ ]+]] = alloca [1 x i8], align 1
; CHECK: %[[INT_BITS:[^ ]+]] = alloca [1 x i64], align 8
; CHECK: store i8 1, ptr %{{[^,]+}}, align 1
; CHECK: store i64 %value, ptr %{{[^,]+}}, align 8
; CHECK: %[[INT_RESULT:[^ ]+]] = alloca i64, align 8
; CHECK: %[[INT_APPLIED:[^ ]+]] = call i1 @ir_hotfix_invoke(i64 3982964892787487464, i64 -6621453603226705439, ptr %{{[^,]+}}, ptr %{{[^,]+}}, i32 1, ptr null, ptr %[[INT_RESULT]])
; CHECK: br i1 %[[INT_APPLIED]], label %[[INT_PATCHED:[^,]+]], label %[[INT_FALLBACK:[^,]+]]
; CHECK: [[INT_PATCHED]]:
; CHECK: %[[INT_VALUE:[^ ]+]] = load i64, ptr %[[INT_RESULT]], align 8
; CHECK: ret i64 %[[INT_VALUE]]
; CHECK: [[INT_FALLBACK]]:
; CHECK: %[[INT_NATIVE:[^ ]+]] = call swiftcc i64 @integerTarget.hotfix_original(i64 signext %value)
; CHECK: ret i64 %[[INT_NATIVE]]

; CHECK-LABEL: define swiftcc i1 @booleanTarget(
; CHECK-SAME: i1 zeroext %value) #[[INTEGER_ATTRS]] {
; CHECK: store i8 2, ptr %{{[^,]+}}, align 1
; CHECK: %[[BOOL_BITS_VALUE:[^ ]+]] = zext i1 %value to i64
; CHECK: store i64 %[[BOOL_BITS_VALUE]], ptr %{{[^,]+}}, align 8
; CHECK: %[[BOOL_APPLIED:[^ ]+]] = call i1 @ir_hotfix_invoke(i64 -1784015009137783678, i64 -7903164660095746219, ptr %{{[^,]+}}, ptr %{{[^,]+}}, i32 1, ptr null, ptr %[[BOOL_RESULT:[^ )]+]])
; CHECK: br i1 %[[BOOL_APPLIED]], label %[[BOOL_PATCHED:[^,]+]], label %[[BOOL_FALLBACK:[^,]+]]
; CHECK: [[BOOL_PATCHED]]:
; CHECK: %[[BOOL_RESULT_BITS:[^ ]+]] = load i64, ptr %[[BOOL_RESULT]], align 8
; CHECK: %[[BOOL_VALUE:[^ ]+]] = trunc i64 %[[BOOL_RESULT_BITS]] to i1
; CHECK: ret i1 %[[BOOL_VALUE]]
; CHECK: [[BOOL_FALLBACK]]:
; CHECK: %[[BOOL_NATIVE:[^ ]+]] = call swiftcc i1 @booleanTarget.hotfix_original(i1 zeroext %value)
; CHECK: ret i1 %[[BOOL_NATIVE]]

; CHECK-LABEL: define swiftcc void @voidTarget(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK: store i8 1, ptr %{{[^,]+}}, align 1
; CHECK: store i64 %value, ptr %{{[^,]+}}, align 8
; CHECK: %[[VOID_APPLIED:[^ ]+]] = call i1 @ir_hotfix_invoke(i64 525488853093286690, i64 -4672385284892258968, ptr %{{[^,]+}}, ptr %{{[^,]+}}, i32 1, ptr null, ptr null)
; CHECK: br i1 %[[VOID_APPLIED]], label %[[VOID_PATCHED:[^,]+]], label %[[VOID_FALLBACK:[^,]+]]
; CHECK: [[VOID_PATCHED]]:
; CHECK-NEXT: ret void
; CHECK: [[VOID_FALLBACK]]:
; CHECK: call swiftcc void @voidTarget.hotfix_original(i64 signext %value)
; CHECK: ret void

; CHECK-LABEL: define swiftcc i64 @recursiveTarget(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK: call i1 @ir_hotfix_invoke(i64 891724747207399162, i64 -6621453603226705439,
; CHECK: call swiftcc i64 @recursiveTarget.hotfix_original(i64 signext %value)

; CHECK-LABEL: define swiftcc i64 @semanticTarget(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: call i1 @ir_hotfix_invoke
; CHECK: call swiftcc i64 @semanticTarget.hotfix_original(i64 %value)

; CHECK-LABEL: define internal swiftcc i64 @internalTarget(
; CHECK: call i1 @ir_hotfix_invoke
; CHECK: call swiftcc i64 @internalTarget.hotfix_original(i64 %value)

; CHECK-LABEL: define swiftcc i64 @returnedTarget(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: call i1 @ir_hotfix_invoke
; CHECK: call swiftcc i64 @returnedTarget.hotfix_original(i64 returned %value)

; CHECK-LABEL: define swiftcc i64 @rangeTarget(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: call i1 @ir_hotfix_invoke
; CHECK: call swiftcc range(i64 0, 10) i64 @rangeTarget.hotfix_original(i64 %value)

; CHECK-LABEL: define swiftcc double @unsupported(double %value) {
; CHECK-NEXT: entry:
; CHECK-NEXT: %sum = fadd double %value, 1.000000e+00
; CHECK-NEXT: ret double %sum
; CHECK-NEXT: }

; CHECK: declare swiftcc i64 @externalTarget(i64 signext)

; CHECK-LABEL: define swiftcc i64 @ir_hotfix_helper(i64 %value) {
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %value
; CHECK-NEXT: }

; CHECK-LABEL: define private swiftcc i64 @already.hotfix_original(i64 %value) {
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %value
; CHECK-NEXT: }

; CHECK: declare i1 @ir_hotfix_invoke(i64, i64, ptr, ptr, i32, ptr, ptr)

; CHECK-LABEL: define private swiftcc i64 @integerTarget.hotfix_original(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK-NEXT: entry:
; CHECK-NEXT: %sum = add i64 %value, 1
; CHECK-NEXT: ret i64 %sum
; CHECK-NEXT: }

; CHECK-LABEL: define private swiftcc i1 @booleanTarget.hotfix_original(
; CHECK-SAME: i1 zeroext %value) #[[INTEGER_ATTRS]] {
; CHECK: ret i1 %value

; CHECK-LABEL: define private swiftcc void @voidTarget.hotfix_original(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK: ret void

; CHECK-LABEL: define private swiftcc i64 @recursiveTarget.hotfix_original(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK: %recursive = call swiftcc i64 @recursiveTarget.hotfix_original(i64 signext %next)
; CHECK-NOT: call swiftcc i64 @recursiveTarget(i64
; CHECK: %result = add i64 %recursive, 1
; CHECK: ret i64 %result

; CHECK-LABEL: define private swiftcc i64 @semanticTarget.hotfix_original(
; CHECK-SAME: i64 %value) #[[SEMANTIC_ATTRS:[0-9]+]] {
; CHECK: ret i64 %sum

; CHECK-LABEL: define private swiftcc i64 @internalTarget.hotfix_original(
; CHECK: ret i64 %value

; CHECK-LABEL: define private swiftcc i64 @returnedTarget.hotfix_original(
; CHECK-SAME: i64 returned %value) #[[INTEGER_ATTRS]] {
; CHECK: ret i64 %value

; CHECK-LABEL: define private swiftcc range(i64 0, 10) i64 @rangeTarget.hotfix_original(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: ret i64 %masked

; CHECK: attributes #[[INTEGER_ATTRS]] = { noinline }
; CHECK: attributes #[[SEMANTIC_ATTRS]] = { nofree noinline nosync memory(none) }

; NO-CLONES-NOT: @unsupported.hotfix_original
; NO-CLONES-NOT: @externalTarget.hotfix_original
; NO-CLONES-NOT: @ir_hotfix_helper.hotfix_original
; NO-CLONES-NOT: @already.hotfix_original.hotfix_original
; NO-CLONES-NOT: @variadicTarget.hotfix_original

; AUTO: define swiftcc i64 @integerTarget
; AUTO: call i1 @ir_hotfix_invoke
; AUTO: define private swiftcc i64 @integerTarget.hotfix_original

; OPT-LABEL: define i64 @callSemanticTwice(
; OPT: %first = {{(tail )?}}call swiftcc i64 @semanticTarget(i64 %value)
; OPT: %second = {{(tail )?}}call swiftcc i64 @semanticTarget(i64 %value)
; OPT: %sum = add i64 %second, %first
; OPT: ret i64 %sum

; OPT-LABEL: define i1 @observeReturned(
; OPT: %patched = {{(tail )?}}call swiftcc i64 @returnedTarget(i64 %value)
; OPT: %different = icmp ne i64 %patched, %value
; OPT: ret i1 %different

; OPT-LABEL: define i1 @observeRange(
; OPT: %patched = {{(tail )?}}call swiftcc i64 @rangeTarget(i64 %value)
; OPT: %in.range = icmp ult i64 %patched, 10
; OPT: ret i1 %in.range

source_filename = "scalars.ll"

define swiftcc i64 @integerTarget(i64 signext %value) alwaysinline {
entry:
  %sum = add i64 %value, 1
  ret i64 %sum
}

define swiftcc i1 @booleanTarget(i1 zeroext %value) {
entry:
  ret i1 %value
}

define swiftcc void @voidTarget(i64 signext %value) {
entry:
  ret void
}

define swiftcc i64 @recursiveTarget(i64 signext %value) {
entry:
  %done = icmp eq i64 %value, 0
  br i1 %done, label %base, label %recurse

base:
  ret i64 0

recurse:
  %next = sub i64 %value, 1
  %recursive = call swiftcc i64 @recursiveTarget(i64 signext %next)
  %result = add i64 %recursive, 1
  ret i64 %result
}

define swiftcc i64 @semanticTarget(i64 %value) nofree nosync memory(none) {
entry:
  %sum = add i64 %value, 2
  ret i64 %sum
}

define i64 @callSemanticTwice(i64 %value) {
entry:
  %first = call swiftcc i64 @semanticTarget(i64 %value)
  %second = call swiftcc i64 @semanticTarget(i64 %value)
  %sum = add i64 %first, %second
  ret i64 %sum
}

define internal swiftcc i64 @internalTarget(i64 %value) {
entry:
  ret i64 %value
}

define swiftcc i64 @returnedTarget(i64 returned %value) {
entry:
  ret i64 %value
}

define i1 @observeReturned(i64 %value) {
entry:
  %patched = call swiftcc i64 @returnedTarget(i64 %value)
  %different = icmp ne i64 %patched, %value
  ret i1 %different
}

define swiftcc range(i64 0, 10) i64 @rangeTarget(i64 %value) {
entry:
  %masked = urem i64 %value, 10
  ret i64 %masked
}

define i1 @observeRange(i64 %value) {
entry:
  %patched = call swiftcc i64 @rangeTarget(i64 %value)
  %in.range = icmp ult i64 %patched, 10
  ret i1 %in.range
}

define swiftcc double @unsupported(double %value) {
entry:
  %sum = fadd double %value, 1.000000e+00
  ret double %sum
}

declare swiftcc i64 @externalTarget(i64 signext)

define swiftcc i64 @ir_hotfix_helper(i64 %value) {
entry:
  ret i64 %value
}

define private swiftcc i64 @already.hotfix_original(i64 %value) {
entry:
  ret i64 %value
}

define swiftcc i64 @variadicTarget(i64 %value, ...) {
entry:
  ret i64 %value
}
