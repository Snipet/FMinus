; fminus-build writes this machine's assembly, optimised IR and objects; the gain loop becomes NEON multiplies.
; REQUIRES: arm64
; RUN: fminus-build %s --emit asm -o - | FileCheck %s --check-prefix=ASM
; RUN: fminus-build %s --emit ll -O2 -o - | FileCheck %s --check-prefix=OPT
; RUN: fminus-build %s --emit obj -o %t.o
; RUN: llvm-objdump -t %t.o | FileCheck %s --check-prefix=SYM
; RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=OBJ
; ASM-LABEL: _gain:
; ASM: {{fmul(\.4s| v[0-9]+\.4s)}}
; OPT: vector.body:
; SYM: _gain
; OBJ: {{fmul(\.4s| v[0-9]+\.4s)}}

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
