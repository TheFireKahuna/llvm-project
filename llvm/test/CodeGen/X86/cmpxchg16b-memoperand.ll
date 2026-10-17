; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mattr=+cx16 -stop-after=finalize-isel < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mattr=+cx16 -stop-after=x86-expand-pseudo < %s | FileCheck %s

; The cmpxchg16b the pseudos become keeps the exchange's memory operand, with
; RBX free and with RBX as the base pointer.

; CHECK-LABEL: name: cas16
; CHECK: LCMPXCHG16B {{.*}} :: (load store acq_rel acquire (s128) on %ir.p)
define { i128, i1 } @cas16(ptr %p, i128 %old, i128 %new) {
  %r = cmpxchg ptr %p, i128 %old, i128 %new acq_rel acquire, align 16
  ret { i128, i1 } %r
}

; CHECK-LABEL: name: cas16_rbx_base
; CHECK: LCMPXCHG16B{{(_SAVE_RBX)?}} {{.*}} :: (load store acq_rel acquire (s128) on %ir.b)
define { i128, i1 } @cas16_rbx_base(i64 %n, i128 %old, i128 %new) "frame-pointer"="all" {
  %a = alloca i8, i64 %n, align 64
  %b = alloca i128, align 64
  %r = cmpxchg ptr %b, i128 %old, i128 %new acq_rel acquire, align 16
  call void @use(ptr %a)
  ret { i128, i1 } %r
}

declare void @use(ptr)
