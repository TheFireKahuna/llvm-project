; RUN: %lli -jit-kind=mcjit %s %t.mcjit | FileCheck %s
; RUN: FileCheck %s --input-file=%t.mcjit --check-prefix=FILE
; RUN: %lli %s %t.orc | FileCheck %s
; RUN: FileCheck %s --input-file=%t.orc --check-prefix=FILE

; The C runtime functions that JIT'd code finds in the process share one set
; of streams, whichever of them the host has to define itself: what puts and
; printf write appears in the order written, and fprintf writes to a stream
; that fopen returned, here more than the stream buffers.

; CHECK:      first
; CHECK-NEXT: second 2
; CHECK-NEXT: third
; FILE: {{^ +4$}}

@first = private constant [6 x i8] c"first\00"
@second = private constant [11 x i8] c"second %d\0A\00"
@third = private constant [6 x i8] c"third\00"
@mode = private constant [2 x i8] c"w\00"
@written = private constant [8 x i8] c"%5000d\0A\00"

declare i32 @puts(ptr)
declare i32 @printf(ptr, ...)
declare ptr @fopen(ptr, ptr)
declare i32 @fprintf(ptr, ptr, ...)
declare i32 @fclose(ptr)

define i32 @main(i32 %argc, ptr %argv) {
  call i32 @puts(ptr @first)
  call i32 (ptr, ...) @printf(ptr @second, i32 2)
  call i32 @puts(ptr @third)
  %arg = getelementptr ptr, ptr %argv, i64 1
  %name = load ptr, ptr %arg
  %file = call ptr @fopen(ptr %name, ptr @mode)
  call i32 (ptr, ptr, ...) @fprintf(ptr %file, ptr @written, i32 4)
  %closed = call i32 @fclose(ptr %file)
  ret i32 %closed
}
