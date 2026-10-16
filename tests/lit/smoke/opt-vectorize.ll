; LLVM's tools run under lit: opt's O3 pipeline vectorises a gain loop for the host target.
; RUN: opt -mtriple=%host_triple -passes='default<O3>' -S %s | FileCheck %s
; CHECK-LABEL: define void @gain(
; CHECK: vector.body:
; CHECK: fmul <{{[0-9]+}} x float>

define void @gain(ptr noalias %buf, i64 %n, float %g) {
entry:
  %any = icmp sgt i64 %n, 0
  br i1 %any, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr inbounds float, ptr %buf, i64 %i
  %x = load float, ptr %p, align 4
  %y = fmul float %x, %g
  store float %y, ptr %p, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
