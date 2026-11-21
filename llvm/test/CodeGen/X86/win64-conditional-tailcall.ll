; RUN: llc -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,V2
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,V2
; RUN: llc -mtriple=x86_64-pc-windows-msvc -x86-wineh-unwindv2-force-mode=2 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,V2
; RUN: llc -mtriple=x86_64-pc-windows-msvc -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,V1
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -x86-wineh-unwindv2-force-mode=1 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,V1
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -x86-wineh-unwindv2-force-mode=0 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=CHECK,V1
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --unwind - | FileCheck %s --check-prefix=UNWIND

; A function with Windows unwind info makes a conditional tail call only where
; it requires unwind version 2: a version 1 unwinder finds an epilogue by
; reading the code and does not take a jcc for one, a version 2 unwinder
; reads no code. The jcc lies in a shrink-wrapped function's leaf code, ahead
; of its unwind range or after it, as the jmp it replaces did. A function
; with no unwind info makes one under either version.

declare void @f(i32)
declare void @g(i32)
declare void @h()

; CHECK-LABEL: before:
; V2:            ja f # TAILCALL
; V2-NEXT:     .Lunwind_begin0:
; V2-NEXT:     .seh_proc before
; V1:            jbe [[FRAME:.LBB0_[0-9]+]]
; V1-NEXT:     # %bb.{{[0-9]+}}: # %tail
; V1-NEXT:       jmp f # TAILCALL
; V1-NEXT:     .Lunwind_begin0:
; V1-NEXT:     .seh_proc before
; V1-NEXT:     [[FRAME]]:
; CHECK:         callq h
; CHECK:         .seh_endepilogue
; CHECK-NEXT:    jmp g # TAILCALL
; CHECK-NOT:     TAILCALL
; CHECK:         .seh_endproc
define void @before(i32 %x) {
  %c = icmp ugt i32 %x, 10
  br i1 %c, label %tail, label %frame
frame:
  call void @h()
  call void @h()
  tail call void @g(i32 %x)
  ret void
tail:
  tail call void @f(i32 %x)
  ret void
}

; CHECK-LABEL: after:
; V1:          [[TAIL:.LBB1_[0-9]+]]: # %tail
; V1-NEXT:       jmp f # TAILCALL
; CHECK:         .seh_proc after
; CHECK:         callq h
; CHECK:         .seh_endepilogue
; CHECK-NEXT:    jmp [[LEAF:.LBB1_[0-9]+]]
; CHECK-NEXT:    .seh_endproc
; CHECK-NEXT:  [[LEAF]]:
; CHECK-NEXT:    cmpl $10, %e{{cx|di}}
; V2-NEXT:       ja f # TAILCALL
; V1-NEXT:       ja [[TAIL]]
define void @after(i32 %x, i32 %y) {
  %c0 = icmp eq i32 %y, 0
  br i1 %c0, label %frame, label %leaf
frame:
  call void @h()
  br label %leaf
leaf:
  %c = icmp ugt i32 %x, 10
  br i1 %c, label %tail, label %other
other:
  tail call void @g(i32 1)
  ret void
tail:
  tail call void @f(i32 %x)
  ret void
}

; CHECK-LABEL: frameless:
; CHECK-NEXT:  # %bb.0:
; CHECK-NEXT:    cmpl $10, %e{{cx|di}}
; CHECK-NEXT:    ja f # TAILCALL
; CHECK-NEXT:  # %bb.1:
; CHECK-NEXT:    jmp g # TAILCALL
define void @frameless(i32 %x) {
  %c = icmp ugt i32 %x, 10
  br i1 %c, label %tail, label %other
other:
  tail call void @g(i32 %x)
  ret void
tail:
  tail call void @f(i32 %x)
  ret void
}

; The unwind range of each frame excludes the jcc: before's begins after it,
; at +0x9, after's ends at +0x19, ahead of it.
; UNWIND:      StartAddress: before +0x9
; UNWIND-NEXT: EndAddress: before +0x26
; UNWIND-NEXT: UnwindInfoAddress:
; UNWIND-NEXT: UnwindInfo {
; UNWIND-NEXT:   Version: 2
; UNWIND:      StartAddress: after +0x4
; UNWIND-NEXT: EndAddress: after +0x19
; UNWIND-NEXT: UnwindInfoAddress:
; UNWIND-NEXT: UnwindInfo {
; UNWIND-NEXT:   Version: 2
; UNWIND-NOT:  StartAddress
