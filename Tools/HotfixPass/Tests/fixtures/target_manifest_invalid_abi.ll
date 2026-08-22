%struct.HFDescriptor = type { i32, i32, i64, i64, i32, i32, i32, i32, ptr, ptr }

@integer.name = private constant [14 x i8] c"integerTarget\00"
@integer.kinds = private constant [1 x i32] [i32 1]
@integer.descriptor = private constant %struct.HFDescriptor { i32 2, i32 56, i64 3982964892787487464, i64 -6621453603226705439, i32 1, i32 1, i32 0, i32 0, ptr @integer.name, ptr @integer.kinds }, section "__DATA,__hotfix", align 8
