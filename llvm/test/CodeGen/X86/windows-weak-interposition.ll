; A weak definition is the implementation's fallback, and on ELF a strong
; definition elsewhere in the program supersedes it. PE has no such rule, so
; the definition carries an entry that forwards to the program's, and start-up
; fills the pointer it reads. Call sites are untouched.

; RUN: llc -O2 -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -O2 -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O2 -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefix=OTHER
; RUN: llc -O2 -mtriple=x86_64-w64-windows-gnu < %s | FileCheck %s --check-prefix=OTHER

declare dllimport ptr @malloc(i64)

define weak ptr @weakfn(i64 %n) {
  %p = call ptr @malloc(i64 %n)
  ret ptr %p
}

; CHECK-LABEL: weakfn:
; CHECK:       movq .L__interpose.weakfn+16(%rip), %rax
; CHECK-NEXT:  testq %rax, %rax
; An image whose program replaced nothing takes neither the branch nor the
; instructions behind it: the body falls through, and the forward is laid out
; past the return.
; CHECK-NEXT:  jne
; CHECK:       callq *__imp_malloc(%rip)
; CHECK:       retq
; The forward is a tail call, so a body that needed no frame does not grow one.
; CHECK:       jmpq *%rax

; A call site is a plain direct call.
define ptr @caller(i64 %n) {
  %p = call ptr @weakfn(i64 %n)
  ret ptr %p
}
; CHECK-LABEL: caller:
; CHECK: callq weakfn

; linkonce is a deduplication rule, not a statement that the program may
; supersede the definition; covering it would put an indirection in front of
; every inline call.
define linkonce_odr i32 @linkoncefn(i32 %x) {
  %r = add i32 %x, 1
  ret i32 %r
}
; CHECK-LABEL: linkoncefn:
; CHECK-NOT: __interpose

; A variadic function cannot pass its arguments on unchanged.
define weak i32 @variadic(ptr %fmt, ...) {
  ret i32 0
}
; CHECK-LABEL: variadic:
; CHECK-NOT: __interpose

; The record is read-only in the image; start-up opens the page for the one
; store and closes it again. It carries the 128-bit hash of the name, the
; pointer start-up fills, and the function's own address, which is what keeps
; a definition from forwarding to itself. The linker bounds the run, so the
; record needs no symbol of its own and two objects defining the same weak
; function do not collide over one.
; CHECK: .section .wkintp,"dr"
; CHECK: .L__interpose.weakfn:
; CHECK-NEXT: .quad
; CHECK-NEXT: .quad
; CHECK-NEXT: .quad 0
; CHECK-NEXT: .quad weakfn

; No record for either of the two that do not take part.
; CHECK-NOT: __interpose.linkoncefn
; CHECK-NOT: __interpose.variadic

; Every other COFF target keeps the plain definition.
; OTHER-NOT: __interpose
