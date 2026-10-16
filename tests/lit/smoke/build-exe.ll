; fminus-build links an executable with the FMinus runtime, and the executable runs.
; RUN: fminus-build %s -o %t
; RUN: %t | FileCheck %s
; CHECK: runtime abi {{[0-9]+}}

@format = private unnamed_addr constant [16 x i8] c"runtime abi %d\0A\00"

declare i32 @printf(ptr, ...)
declare i32 @fminus_rt_abi_version()

define i32 @main() {
  %abi = call i32 @fminus_rt_abi_version()
  %written = call i32 (ptr, ...) @printf(ptr @format, i32 %abi)
  ret i32 0
}
