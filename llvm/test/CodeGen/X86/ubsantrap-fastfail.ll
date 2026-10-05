; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s | \
; RUN:   llvm-objdump -d - | FileCheck --check-prefix=OBJ %s
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck --check-prefix=MSVC %s

; On Windows Itanium and NT-POSIX, a sanitizer trap whose kind is 64 or more
; fails fast with the kind as its code; other kinds stay ud1.

; CHECK-LABEL: f:
; CHECK:       movl $64, %ecx
; CHECK-NEXT:  int $41
; CHECK:       movl $65, %ecx
; CHECK-NEXT:  int $41
; CHECK:       ud1l 2(%eax), %eax
; OBJ:         b9 40 00 00 00 movl $0x40, %ecx
; OBJ-NEXT:    cd 29 int $0x29
; MSVC:        ud1l 64(%eax), %eax
; MSVC:        ud1l 65(%eax), %eax
define void @f(i1 %c, i1 %d) {
  br i1 %c, label %t1, label %n
t1:
  call void @llvm.ubsantrap(i8 64) nomerge
  unreachable
n:
  br i1 %d, label %t2, label %e
t2:
  call void @llvm.ubsantrap(i8 65) nomerge
  unreachable
e:
  call void @llvm.ubsantrap(i8 2) nomerge
  unreachable
}

declare void @llvm.ubsantrap(i8)
