; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s --check-prefix=MSVC
; RUN: llc < %s -mtriple=i686-pc-windows-msvc | FileCheck %s --check-prefix=X86
; RUN: llc < %s -mtriple=x86_64-w64-windows-gnu | FileCheck %s --check-prefix=MINGW
; RUN: sed -e 's/"cfguard"/"other"/' %s | llc -mtriple=x86_64-pc-windows-msvc \
; RUN:   | FileCheck %s --check-prefix=NOCFG

; A function suppressed as a call target is named to the linker as MSVC names
; it, with /GUARDSYM:<symbol>,S, so the guard table lists it as not valid. A
; declaration names nothing.

; MSVC:      .section .drectve
; MSVC-NEXT: .ascii " /GUARDSYM:suppressed,S"
; MSVC-NOT:  GUARDSYM
; X86:       .ascii " /GUARDSYM:_suppressed,S"
; MINGW:     .ascii " -guardsym:suppressed,S"
; NOCFG-NOT: GUARDSYM

define void @suppressed() #0 {
  call void @declared()
  ret void
}

declare void @declared() #0

attributes #0 = { "guard_suppress" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}
