; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s

; NT-POSIX runs static constructors and destructors from the .CRT$XC* and
; .CRT$XT* sections, as the MSVC and Itanium environments do, not from .ctors
; and .dtors.

@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @ctor, ptr null },
  { i32, ptr, ptr } { i32 200, ptr @early, ptr null }
]
@llvm.global_dtors = appending global [1 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @dtor, ptr null }
]

define void @ctor() {
  ret void
}

define void @early() {
  ret void
}

define void @dtor() {
  ret void
}

; CHECK-NOT: .ctors
; CHECK:      .section .CRT$XCC,"dr"
; CHECK-NEXT: .p2align 3
; CHECK-NEXT: .xword early
; CHECK:      .section .CRT$XCU,"dr"
; CHECK-NEXT: .p2align 3
; CHECK-NEXT: .xword ctor
; CHECK:      .section .CRT$XTX,"dr"
; CHECK-NEXT: .p2align 3
; CHECK-NEXT: .xword dtor
; CHECK-NOT: .dtors
