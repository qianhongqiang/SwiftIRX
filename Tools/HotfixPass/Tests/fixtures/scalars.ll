; RUN: opt -load-pass-plugin %plugin -passes=hotfix-instrument -S %s | FileCheck %s

; CHECK: %struct.HFDescriptor = type { i32, i32, i64, i64, i32, i32, i32, i32, ptr, ptr }
; CHECK: %struct.HFValue = type { i32, i32, i64, ptr, i64 }
; CHECK: %struct.HFPatchFrame = type { i32, i32, i64, i64, ptr, i32, i32, %struct.HFHandle, %struct.HFValue, i32, i32 }
; CHECK: %struct.HFHandle = type { i64, i32, i16, i16 }

; CHECK: @__ir_hotfix_name.37465577a37332e8.a41bdf07a2f121e1 = private constant [14 x i8] c"integerTarget\00"
; CHECK: @__ir_hotfix_kinds.37465577a37332e8.a41bdf07a2f121e1 = private constant [1 x i32] [i32 1]
; CHECK: @__ir_hotfix_descriptor.37465577a37332e8.a41bdf07a2f121e1 = private constant %struct.HFDescriptor { i32 2, i32 56, i64 3982964892787487464, i64 -6621453603226705439, i32 1, i32 1, i32 0, i32 0, ptr @__ir_hotfix_name.37465577a37332e8.a41bdf07a2f121e1, ptr @__ir_hotfix_kinds.37465577a37332e8.a41bdf07a2f121e1 }, section "__DATA,__hotfix"
; CHECK: @__ir_hotfix_name.e73de7cd22608c82.9252519d49927f55 = private constant [14 x i8] c"booleanTarget\00"
; CHECK: @__ir_hotfix_kinds.e73de7cd22608c82.9252519d49927f55 = private constant [1 x i32] [i32 2]
; CHECK: @__ir_hotfix_descriptor.e73de7cd22608c82.9252519d49927f55 = private constant %struct.HFDescriptor { i32 2, i32 56, i64 -1784015009137783678, i64 -7903164660095746219, i32 2, i32 1, i32 0, i32 0, ptr @__ir_hotfix_name.e73de7cd22608c82.9252519d49927f55, ptr @__ir_hotfix_kinds.e73de7cd22608c82.9252519d49927f55 }, section "__DATA,__hotfix"
; CHECK: @__ir_hotfix_name.074ae953e60d8322.bf285a56b83c0d68 = private constant [11 x i8] c"voidTarget\00"
; CHECK: @__ir_hotfix_descriptor.074ae953e60d8322.bf285a56b83c0d68 = private constant %struct.HFDescriptor { i32 2, i32 56, i64 525488853093286690, i64 -4672385284892258968, i32 3, i32 1, i32 0, i32 0, ptr @__ir_hotfix_name.074ae953e60d8322.bf285a56b83c0d68, ptr @__ir_hotfix_kinds.074ae953e60d8322.bf285a56b83c0d68 }, section "__DATA,__hotfix"
; CHECK: @__ir_hotfix_name.4eaa70cef627ccb3.a41bde07a2f1202e = private constant [15 x i8] c"instanceTarget\00"
; CHECK: @__ir_hotfix_kinds.4eaa70cef627ccb3.a41bde07a2f1202e = private constant [1 x i32] [i32 1]
; CHECK: @__ir_hotfix_descriptor.4eaa70cef627ccb3.a41bde07a2f1202e = private constant %struct.HFDescriptor { i32 2, i32 56, i64 5668467115194567859, i64 -6621454702738333650, i32 1, i32 1, i32 1, i32 0, ptr @__ir_hotfix_name.4eaa70cef627ccb3.a41bde07a2f1202e, ptr @__ir_hotfix_kinds.4eaa70cef627ccb3.a41bde07a2f1202e }, section "__DATA,__hotfix"
; CHECK: @__ir_hotfix_name.{{[0-9a-f]+}}.{{[0-9a-f]+}} = private constant [21 x i8] c"eightScalarArguments\00"
; CHECK: @__ir_hotfix_kinds.{{[0-9a-f]+}}.{{[0-9a-f]+}} = private constant [8 x i32] [i32 1, i32 2, i32 1, i32 2, i32 1, i32 2, i32 1, i32 2]
; CHECK: @__ir_hotfix_descriptor.{{[0-9a-f]+}}.{{[0-9a-f]+}} = private constant %struct.HFDescriptor { i32 2, i32 56, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, i32 1, i32 8, i32 1, i32 0, ptr @__ir_hotfix_name.{{[0-9a-f]+}}.{{[0-9a-f]+}}, ptr @__ir_hotfix_kinds.{{[0-9a-f]+}}.{{[0-9a-f]+}} }, section "__DATA,__hotfix"
; CHECK: @llvm.used = appending global [10 x ptr]
; CHECK-SAME: ptr @__ir_hotfix_descriptor.37465577a37332e8.a41bdf07a2f121e1,
; CHECK-SAME: ptr @__ir_hotfix_descriptor.e73de7cd22608c82.9252519d49927f55,
; CHECK-SAME: ptr @__ir_hotfix_descriptor.074ae953e60d8322.bf285a56b83c0d68,
; CHECK-SAME: ptr @__ir_hotfix_descriptor.4eaa70cef627ccb3.a41bde07a2f1202e,
; CHECK-SAME: ], section "llvm.metadata"

; CHECK-LABEL: define swiftcc i64 @integerTarget(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS:[0-9]+]] {
; CHECK: %[[INT_ARGUMENTS:[^ ]+]] = alloca [1 x %struct.HFValue], align 8
; CHECK: store i32 1, ptr %{{[^,]+}}, align 4
; CHECK: store i64 %value, ptr %{{[^,]+}}, align 8
; CHECK: %[[INT_FRAME:[^ ]+]] = alloca %struct.HFPatchFrame, align 8
; CHECK: store i32 2, ptr %{{[^,]+}}, align 4
; CHECK: store i32 96, ptr %{{[^,]+}}, align 4
; CHECK: store i64 3982964892787487464, ptr %{{[^,]+}}, align 8
; CHECK: store i64 -6621453603226705439, ptr %{{[^,]+}}, align 8
; CHECK: store i32 1, ptr %{{[^,]+}}, align 4
; CHECK: %[[INT_STATUS:[^ ]+]] = call i32 @hf_vm_invoke(ptr %[[INT_FRAME]])
; CHECK: %[[INT_APPLIED:[^ ]+]] = icmp eq i32 %[[INT_STATUS]], 0
; CHECK: br i1 %[[INT_APPLIED]], label %[[INT_PATCHED:[^,]+]], label %[[INT_FALLBACK:[^,]+]]
; CHECK: [[INT_PATCHED]]:
; CHECK: %[[INT_VALUE:[^ ]+]] = load i64, ptr %{{[^,]+}}, align 8
; CHECK: ret i64 %[[INT_VALUE]]
; CHECK: [[INT_FALLBACK]]:
; CHECK: %[[INT_NATIVE:[^ ]+]] = call swiftcc i64 @integerTarget.hotfix_original(i64 signext %value)
; CHECK: ret i64 %[[INT_NATIVE]]

; CHECK-LABEL: define swiftcc i1 @booleanTarget(
; CHECK-SAME: i1 zeroext %value) #[[INTEGER_ATTRS]] {
; CHECK: store i32 2, ptr %{{[^,]+}}, align 4
; CHECK: %[[BOOL_BITS_VALUE:[^ ]+]] = zext i1 %value to i64
; CHECK: store i64 %[[BOOL_BITS_VALUE]], ptr %{{[^,]+}}, align 8
; CHECK: %[[BOOL_FRAME:[^ ]+]] = alloca %struct.HFPatchFrame, align 8
; CHECK: store i64 -1784015009137783678, ptr %{{[^,]+}}, align 8
; CHECK: store i64 -7903164660095746219, ptr %{{[^,]+}}, align 8
; CHECK: %[[BOOL_STATUS:[^ ]+]] = call i32 @hf_vm_invoke(ptr %[[BOOL_FRAME]])
; CHECK: %[[BOOL_APPLIED:[^ ]+]] = icmp eq i32 %[[BOOL_STATUS]], 0
; CHECK: br i1 %[[BOOL_APPLIED]], label %[[BOOL_PATCHED:[^,]+]], label %[[BOOL_FALLBACK:[^,]+]]
; CHECK: [[BOOL_PATCHED]]:
; CHECK: %[[BOOL_RESULT_BITS:[^ ]+]] = load i64, ptr %{{[^,]+}}, align 8
; CHECK: %[[BOOL_VALUE:[^ ]+]] = trunc i64 %[[BOOL_RESULT_BITS]] to i1
; CHECK: ret i1 %[[BOOL_VALUE]]
; CHECK: [[BOOL_FALLBACK]]:
; CHECK: %[[BOOL_NATIVE:[^ ]+]] = call swiftcc i1 @booleanTarget.hotfix_original(i1 zeroext %value)
; CHECK: ret i1 %[[BOOL_NATIVE]]

; CHECK-LABEL: define swiftcc void @voidTarget(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK: store i32 1, ptr %{{[^,]+}}, align 4
; CHECK: store i64 %value, ptr %{{[^,]+}}, align 8
; CHECK: %[[VOID_FRAME:[^ ]+]] = alloca %struct.HFPatchFrame, align 8
; CHECK: store i64 525488853093286690, ptr %{{[^,]+}}, align 8
; CHECK: store i64 -4672385284892258968, ptr %{{[^,]+}}, align 8
; CHECK: %[[VOID_STATUS:[^ ]+]] = call i32 @hf_vm_invoke(ptr %[[VOID_FRAME]])
; CHECK: %[[VOID_APPLIED:[^ ]+]] = icmp eq i32 %[[VOID_STATUS]], 0
; CHECK: br i1 %[[VOID_APPLIED]], label %[[VOID_PATCHED:[^,]+]], label %[[VOID_FALLBACK:[^,]+]]
; CHECK: [[VOID_PATCHED]]:
; CHECK-NEXT: ret void
; CHECK: [[VOID_FALLBACK]]:
; CHECK: call swiftcc void @voidTarget.hotfix_original(i64 signext %value)
; CHECK: ret void

; CHECK-LABEL: define swiftcc i64 @instanceTarget(
; CHECK-SAME: i64 %value, ptr swiftself %self) #[[INTEGER_ATTRS]] {
; CHECK: %[[INSTANCE_ARGUMENTS:[^ ]+]] = alloca [1 x %struct.HFValue], align 8
; CHECK: store i32 1, ptr %{{[^,]+}}, align 4
; CHECK: store i64 %value, ptr %{{[^,]+}}, align 8
; CHECK: %[[INSTANCE_FRAME:[^ ]+]] = alloca %struct.HFPatchFrame, align 8
; CHECK: store i32 1, ptr %{{[^,]+}}, align 4
; CHECK: %[[RECEIVER_TOKEN:[^ ]+]] = ptrtoint ptr %self to i64
; CHECK: store i64 %[[RECEIVER_TOKEN]], ptr %{{[^,]+}}, align 8
; CHECK: store i16 1, ptr %{{[^,]+}}, align 2
; CHECK: store i16 -32767, ptr %{{[^,]+}}, align 2
; CHECK: %[[INSTANCE_STATUS:[^ ]+]] = call i32 @hf_vm_invoke(ptr %[[INSTANCE_FRAME]])
; CHECK: %[[INSTANCE_APPLIED:[^ ]+]] = icmp eq i32 %[[INSTANCE_STATUS]], 0
; CHECK: br i1 %[[INSTANCE_APPLIED]], label %[[INSTANCE_PATCHED:[^,]+]], label %[[INSTANCE_FALLBACK:[^,]+]]
; CHECK: [[INSTANCE_PATCHED]]:
; CHECK: %[[INSTANCE_VALUE:[^ ]+]] = load i64, ptr %{{[^,]+}}, align 8
; CHECK: ret i64 %[[INSTANCE_VALUE]]
; CHECK: [[INSTANCE_FALLBACK]]:
; CHECK: %[[INSTANCE_NATIVE:[^ ]+]] = call swiftcc i64 @instanceTarget.hotfix_original(i64 %value, ptr nocapture readnone swiftself %self)
; CHECK: ret i64 %[[INSTANCE_NATIVE]]

; CHECK-LABEL: define swiftcc i64 @eightScalarArguments(
; CHECK: alloca [8 x %struct.HFValue], align 8
; CHECK: call i32 @hf_vm_invoke(ptr %{{[^)]+}})
; CHECK: call swiftcc i64 @eightScalarArguments.hotfix_original(

; CHECK-LABEL: define swiftcc i64 @nineScalarArguments(
; CHECK-NOT: call i32 @hf_vm_invoke
; CHECK-NOT: @nineScalarArguments.hotfix_original
; CHECK: ret i64 %a

; CHECK-LABEL: define swiftcc i64 @unverifiedReceiver(
; CHECK-SAME: i64 %value, ptr nocapture readnone swiftself %self) {
; CHECK-NEXT: entry:
; CHECK-NEXT: %sum = add i64 %value, 4
; CHECK-NEXT: ret i64 %sum
; CHECK-NEXT: }

; CHECK-LABEL: define swiftcc i64 @recursiveTarget(
; CHECK-SAME: i64 signext %value) #[[INTEGER_ATTRS]] {
; CHECK: store i64 891724747207399162, ptr %{{[^,]+}}, align 8
; CHECK: store i64 -6621453603226705439, ptr %{{[^,]+}}, align 8
; CHECK: call i32 @hf_vm_invoke(ptr %{{[^)]+}})
; CHECK: call swiftcc i64 @recursiveTarget.hotfix_original(i64 signext %value)

; CHECK-LABEL: define swiftcc i64 @semanticTarget(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: call i32 @hf_vm_invoke
; CHECK: call swiftcc i64 @semanticTarget.hotfix_original(i64 %value)

; CHECK-LABEL: define internal swiftcc i64 @internalTarget(
; CHECK: call i32 @hf_vm_invoke
; CHECK: call swiftcc i64 @internalTarget.hotfix_original(i64 %value)

; CHECK-LABEL: define swiftcc i64 @returnedTarget(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: call i32 @hf_vm_invoke
; CHECK: call swiftcc i64 @returnedTarget.hotfix_original(i64 returned %value)

; CHECK-LABEL: define swiftcc i64 @rangeTarget(
; CHECK-SAME: i64 %value) #[[INTEGER_ATTRS]] {
; CHECK: call i32 @hf_vm_invoke
; CHECK: call swiftcc range(i64 0, 10) i64 @rangeTarget.hotfix_original(i64 %value)

; CHECK-LABEL: define swiftcc double @unsupported(double %value) {
; CHECK-NEXT: entry:
; CHECK-NEXT: %sum = fadd double %value, 1.000000e+00
; CHECK-NEXT: ret double %sum
; CHECK-NEXT: }

; CHECK-LABEL: define swiftcc i64 @unsupportedPointer(ptr %value) {
; CHECK-NEXT: entry:
; CHECK-NEXT: %bits = ptrtoint ptr %value to i64
; CHECK-NEXT: ret i64 %bits
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

; CHECK: declare i32 @hf_vm_invoke(ptr)

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

; CHECK-LABEL: define private swiftcc i64 @instanceTarget.hotfix_original(
; CHECK-SAME: i64 %value, ptr nocapture readnone swiftself %self) #[[INTEGER_ATTRS]] {
; CHECK: %sum = add i64 %value, 3
; CHECK: ret i64 %sum

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
; NO-CLONES-NOT: @unsupportedPointer.hotfix_original
; NO-CLONES-NOT: @unverifiedReceiver.hotfix_original
; NO-CLONES-NOT: @nineScalarArguments.hotfix_original

; NO-MANIFEST: define swiftcc i64 @integerTarget
; NO-MANIFEST: call i32 @hf_vm_invoke
; NO-MANIFEST-LABEL: define swiftcc i64 @instanceTarget(
; NO-MANIFEST-SAME: i64 %value, ptr nocapture readnone swiftself %self) {
; NO-MANIFEST-NEXT: entry:
; NO-MANIFEST-NEXT: %sum = add i64 %value, 3
; NO-MANIFEST-NEXT: ret i64 %sum
; NO-MANIFEST-NEXT: }

; AUTO: define swiftcc i64 @integerTarget
; AUTO: call i32 @hf_vm_invoke
; AUTO: define swiftcc i64 @eightScalarArguments
; AUTO: alloca [8 x %struct.HFValue], align 8
; AUTO: call i32 @hf_vm_invoke
; AUTO: define swiftcc i64 @nineScalarArguments
; AUTO: define private swiftcc i64 @integerTarget.hotfix_original
; AUTO: define private swiftcc i64 @eightScalarArguments.hotfix_original
; AUTO-NOT: @nineScalarArguments.hotfix_original

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

; RETAIN: @__ir_hotfix_name.4eaa70cef627ccb3.a41bde07a2f1202e = private constant [15 x i8] c"instanceTarget\00"
; RETAIN: @__ir_hotfix_kinds.4eaa70cef627ccb3.a41bde07a2f1202e = private constant [1 x i32] [i32 1]
; RETAIN: @__ir_hotfix_descriptor.4eaa70cef627ccb3.a41bde07a2f1202e = private constant %struct.HFDescriptor {{.*}}section "__DATA,__hotfix"
; RETAIN: @llvm.compiler.used = appending global [20 x ptr]
; RETAIN-SAME: ptr @booleanTarget, ptr @booleanTarget.hotfix_original,
; RETAIN-SAME: ptr @eightScalarArguments, ptr @eightScalarArguments.hotfix_original,
; RETAIN-SAME: ptr @instanceTarget, ptr @instanceTarget.hotfix_original,
; RETAIN-SAME: ptr @integerTarget, ptr @integerTarget.hotfix_original,
; RETAIN-SAME: ptr @internalTarget, ptr @internalTarget.hotfix_original,
; RETAIN-SAME: ptr @rangeTarget, ptr @rangeTarget.hotfix_original,
; RETAIN-SAME: ptr @recursiveTarget, ptr @recursiveTarget.hotfix_original,
; RETAIN-SAME: ptr @returnedTarget, ptr @returnedTarget.hotfix_original,
; RETAIN-SAME: ptr @semanticTarget, ptr @semanticTarget.hotfix_original,
; RETAIN-SAME: ptr @voidTarget, ptr @voidTarget.hotfix_original]
; RETAIN: define swiftcc i64 @integerTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS:[0-9]+]] {
; RETAIN: define swiftcc i1 @booleanTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define swiftcc void @voidTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define swiftcc i64 @instanceTarget(i64 %value, ptr swiftself %self) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: ptrtoint ptr %self to i64
; RETAIN: call i32 @hf_vm_invoke
; RETAIN: call swiftcc i64 @instanceTarget.hotfix_original(i64 %value, ptr nocapture readnone swiftself {{(%self|poison)}})
; RETAIN: define swiftcc i64 @recursiveTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define swiftcc i64 @semanticTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define internal swiftcc i64 @internalTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define swiftcc i64 @returnedTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define swiftcc i64 @rangeTarget({{.*}}) #[[RETAIN_TRAMPOLINE_ATTRS]] {
; RETAIN: define private swiftcc i64 @integerTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS:[0-9]+]] {
; RETAIN: define private swiftcc i1 @booleanTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc void @voidTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc i64 @instanceTarget.hotfix_original(i64 %value, ptr nocapture readnone swiftself %self) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc i64 @recursiveTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc i64 @semanticTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc i64 @internalTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc i64 @returnedTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN: define private swiftcc range(i64 0, 10) i64 @rangeTarget.hotfix_original({{.*}}) #[[RETAIN_CLONE_ATTRS]] {
; RETAIN-NOT: .hotfix_original.
; RETAIN: attributes #[[RETAIN_TRAMPOLINE_ATTRS]] = { noinline }
; RETAIN: attributes #[[RETAIN_CLONE_ATTRS]] = { {{.*}}noinline{{.*}} }

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

define swiftcc i64 @instanceTarget(i64 %value, ptr nocapture readnone swiftself %self) {
entry:
  %sum = add i64 %value, 3
  ret i64 %sum
}

define swiftcc i64 @eightScalarArguments(i64 %a, i1 %b, i64 %c, i1 %d, i64 %e, i1 %f, i64 %g, i1 %h, ptr swiftself %self) {
entry:
  ret i64 %a
}

define swiftcc i64 @nineScalarArguments(i64 %a, i1 %b, i64 %c, i1 %d, i64 %e, i1 %f, i64 %g, i1 %h, i64 %i) {
entry:
  ret i64 %a
}

define swiftcc i64 @unverifiedReceiver(i64 %value, ptr nocapture readnone swiftself %self) {
entry:
  %sum = add i64 %value, 4
  ret i64 %sum
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

define swiftcc i64 @unsupportedPointer(ptr %value) {
entry:
  %bits = ptrtoint ptr %value to i64
  ret i64 %bits
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
