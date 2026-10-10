; RUN: llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s
; RUN: sed -e 's/"cfguard", i32 2/"cfguard", i32 1/' %s \
; RUN:   | llc -mtriple=aarch64-pc-windows-msvc | FileCheck %s
; RUN: sed -e 's/"cfguard"/"other"/' %s | llc -mtriple=aarch64-pc-windows-msvc \
; RUN:   | FileCheck %s --check-prefix=NOCFG

;; Under Control Flow Guard, with checks or with the tables alone, a function
;; that can be a target is aligned to 16 bytes even when optimised for size:
;; the bitmap makes a target off a 16-byte boundary valid together with its
;; whole block. A local function whose address is not taken is never a target
;; and keeps its own alignment.

; CHECK:      .p2align 4
; CHECK-NEXT: visible:
; CHECK:      .p2align 4
; CHECK-NEXT: taken:
; CHECK-NOT:  .p2align 4
; CHECK:      local:

; NOCFG-NOT:  .p2align 4

@fp = global ptr @taken

define void @visible() minsize {
  call void @local()
  ret void
}

define internal void @taken() minsize {
  ret void
}

define internal void @local() minsize noinline {
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}
