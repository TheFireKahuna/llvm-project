; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O2 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -O2 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-w64-windows-gnu -O2 < %s \
; RUN:   | FileCheck --check-prefix=MINGW %s

; On Windows Itanium and NT-POSIX the import pointer of a symbol not known to
; be imported, extern_weak or not, is loaded into a register of its own before
; it is tested, not folded into the compare, so that the linker can replace the
; load with the symbol's address or zero. A call through the pointer keeps its
; memory operand, which the linker rewrites as well. In f, the second compare
; is folded at instruction selection, the first by the peephole optimizer. A
; known import's pointer is folded, as MSVC folds it. Nor is the load narrowed
; where only part of the address is used.

@weakvar = extern_weak global i32
declare extern_weak void @weakfn()
declare void @fn()
declare dllimport void @importfn()
@var = external global i32

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

define i32 @g(ptr %p) {
; CHECK-LABEL: g:
; CHECK:         movq __imp_fn(%rip), [[R1:%r[a-z0-9]+]]
; CHECK-NEXT:    cmpq [[R1]], [[P:%r[a-z]+]]
; CHECK:         movq __imp_var(%rip), [[R2:%r[a-z0-9]+]]
; CHECK-NEXT:    cmpq [[R2]], [[P]]
; CHECK:         cmpq __imp_importfn(%rip), [[P]]
  %a = icmp eq ptr %p, @fn
  %b = icmp eq ptr %p, @var
  %c = icmp eq ptr %p, @importfn
  %ab = or i1 %a, %b
  %abc = or i1 %ab, %c
  %r = zext i1 %abc to i32
  ret i32 %r
}

define i32 @h() {
; CHECK-LABEL: h:
; CHECK:         movq __imp_var(%rip), %rax
; CHECK-NOT:     __imp_var
; CHECK:         retq
  %a = ptrtoint ptr @var to i64
  %t = trunc i64 %a to i32
  ret i32 %t
}
