; ModuleID = 'CombineInstructionsPass'
source_filename = "CombineInstructionsPass"

@__anvill_pc = external global i8

define i64 @CombineAddWithSelect() {
entry:
  %0 = icmp eq i1 0, 0
  %1 = select i1 %0, i64 11, i64 22
  %2 = add i64 add (i64 ptrtoint (i8* @__anvill_pc to i64), i64 100), %1
  ret i64 %2
}

define i64* @CombineCastWithSelect() {
entry:
  %0 = icmp eq i1 0, 0
  %1 = select i1 %0, i64 11, i64 22
  %2 = inttoptr i64 %1 to i64*
  ret i64* %2
}

define i32 @CombineGEPWithSelect() {
entry:
  %0 = alloca i32, align 4
  store i32 0, i32* %0, align 4
  %1 = load i32, i32* %0, align 4
  %2 = icmp eq i32 %1, 0
  %3 = alloca [100 x i32], align 4
  %4 = alloca [100 x i32], align 4
  store [100 x i32] zeroinitializer, [100 x i32]* %4, align 4
  %5 = select i1 %2, [100 x i32]* %3, [100 x i32]* %4
  %6 = getelementptr [100 x i32], [100 x i32]* %5, i32 0, i32 0
  %7 = load i32, i32* %6, align 4
  ret i32 %7
}

define i32 @CombineAddWithPHI() {
entry:
  %0 = alloca i32, align 4
  store i32 0, i32* %0, align 4
  %1 = load i32, i32* %0, align 4
  %2 = icmp eq i32 %1, 0
  br i1 %2, label %first, label %second

first:                                            ; preds = %entry
  %3 = add i32 %1, 1
  br label %exit

second:                                           ; preds = %entry
  %4 = add i32 %1, 2
  br label %exit

exit:                                             ; preds = %second, %first
  %5 = phi i32 [ %3, %first ], [ %4, %second ]
  %6 = add i32 add (i32 ptrtoint (i8* @__anvill_pc to i32), i32 100), %5
  ret i32 %6
}

define i32 @CombineGEPWithPHI() {
entry:
  %0 = alloca i32, align 4
  store i32 0, i32* %0, align 4
  %1 = load i32, i32* %0, align 4
  %2 = icmp eq i32 %1, 0
  br i1 %2, label %first, label %second

first:                                            ; preds = %entry
  %3 = alloca [100 x i32], align 4
  br label %exit

second:                                           ; preds = %entry
  %4 = alloca [100 x i32], align 4
  store [100 x i32] zeroinitializer, [100 x i32]* %4, align 4
  br label %exit

exit:                                             ; preds = %second, %first
  %5 = phi [100 x i32]* [ %3, %first ], [ %4, %second ]
  %6 = getelementptr [100 x i32], [100 x i32]* %5, i32 0, i32 0
  %7 = load i32, i32* %6, align 4
  ret i32 %7
}

define i32 @Combined() {
entry:
  %0 = alloca i32, align 4
  store i32 0, i32* %0, align 4
  %1 = load i32, i32* %0, align 4
  %2 = icmp eq i32 %1, 0
  br i1 %2, label %first, label %second
first:                                            ; preds = %entry
  %3 = alloca [100 x i32], align 4
  br label %exit
second:                                           ; preds = %entry
  %4 = alloca [100 x i32], align 4
  store [100 x i32] zeroinitializer, [100 x i32]* %4, align 4
  br label %exit
exit:                                             ; preds = %second, %first
  %5 = phi [100 x i32]* [ %3, %first ], [ %4, %second ]
  %6 = getelementptr [100 x i32], [100 x i32]* %5, i32 0, i32 0
  %7 = bitcast i32* %6 to i8*
  %8 = ptrtoint i8* %7 to i64
  %9 = add i64 %8, 10
  %10 = trunc i64 %9 to i32
  ret i32 %10
}
