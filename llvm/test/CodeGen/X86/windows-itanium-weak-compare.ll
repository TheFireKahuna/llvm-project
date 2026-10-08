; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O2 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -O2 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-w64-windows-gnu -O2 < %s \
; RUN:   | FileCheck --check-prefix=MINGW %s

; On Windows Itanium and NT-POSIX the import pointer of an extern_weak symbol
; is loaded into a register of its own before it is tested, not folded into
; the compare, so that the linker can replace the load with the symbol's
; address or zero. A call through the pointer keeps its memory operand, which the
; linker rewrites as well. The second compare is folded at instruction
; selection, the first by the peephole optimizer.

@weakvar = extern_weak global i32
declare extern_weak void @weakfn()

define i32 @f() {
; CHECK-LABEL: f:
; CHECK:         movq __imp_weakfn(%rip), %rax
; CHECK-NEXT:    testq %rax, %rax
; CHECK:         callq *__imp_weakfn(%rip)
; CHECK:         movq __imp_weakvar(%rip), %rax
; CHECK-NEXT:    testq %rax, %rax
; MINGW-LABEL: f:
; MINGW:         cmpq $0, .refptr.weakfn(%rip)
; MINGW:         callq *.refptr.weakfn(%rip)
; MINGW:         cmpq $0, .refptr.weakvar(%rip)
  %c = icmp ne ptr @weakfn, null
  br i1 %c, label %call, label %next
call:
  call void @weakfn()
  br label %next
next:
  %d = icmp ne ptr @weakvar, null
  br i1 %d, label %load, label %none
load:
  %v = load i32, ptr @weakvar
  ret i32 %v
none:
  ret i32 0
}
