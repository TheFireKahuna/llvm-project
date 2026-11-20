; RUN: llc -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -mcpu=atom -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,ATOM
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -mcpu=atom -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,ATOM
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -O0 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O0 < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -global-isel -global-isel-abort=2 < %s 2>&1 | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -global-isel -global-isel-abort=2 < %s 2>&1 | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -x86-speculative-load-hardening < %s | FileCheck %s --check-prefixes=CHECK,SLH
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -x86-speculative-load-hardening < %s | FileCheck %s --check-prefixes=CHECK,SLH

; Windows Itanium selects the load of a dispatch's 32-bit table entry as a
; pseudo-instruction that stands for the movslq every other x86-64 triple
; selects, so that the load can be labelled. Every pass treats it as that
; instruction, whatever the options: the dispatch is the same on both triples.

; CHECK-LABEL: dispatch:
; ATOM:          leal (%r{{[a-z]+}}), %eax
; CHECK:         leaq .LJTI0_0(%rip), %r
; CHECK:         movslq ({{%r[a-z]+}},{{%r[a-z]+}},4), %r{{[a-z]+}}
; CHECK-NEXT:    addq {{%r[a-z]+}}, [[A:%r[a-z]+]]
; SLH-NEXT:      orq %rax, [[A]]
; CHECK:         jmpq *[[A]]
define void @dispatch(i32 %x) {
entry:
  switch i32 %x, label %def [
    i32 0, label %l0
    i32 1, label %l1
    i32 2, label %l2
    i32 3, label %l3
  ]
l0:
  call void @f(i32 0)
  ret void
l1:
  call void @f(i32 1)
  ret void
l2:
  call void @f(i32 2)
  ret void
l3:
  call void @f(i32 3)
  ret void
def:
  ret void
}

declare void @f(i32)
