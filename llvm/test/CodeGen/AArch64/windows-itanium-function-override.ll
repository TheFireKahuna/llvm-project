; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; A multiversioned function whose every version needs only features Windows
;; can test as it maps the image is a stub that branches to the version every
;; supported processor runs, with a record of the versions Windows may select
;; instead: tests of each version's capabilities, with those its features
;; imply, in priority order, each ending in a leaf that selects it, then a
;; terminal that keeps the default. A call is direct. One whose versions all
;; lie within the baseline is just the stub. One needing a feature Windows
;; cannot test keeps its cell. Where a version's numbers show only that the
;; processor has a feature, not that it is enabled, their presence selects the
;; jump through the cell that follows the stub, and the resolver chooses.

; CHECK-LABEL: use:
; CHECK:         bl picked
; CHECK:         bl baseline
; CHECK:         bl gated
; CHECK:         adrp [[REG:x[0-9]+]], __imp_cell
; CHECK:         blr [[REG]]

; CHECK:      .p2align 4
; CHECK-NEXT: {{^}}picked:
; CHECK-NEXT: b picked.default
; CHECK-NEXT: .linkfunctionoverride picked, (picked._Msm4, picked._Mlse), (3, 1, 0x1000a), (3, 2, 0x1000b), (2, 2, 0x0), (5, 4, 0x10007), (4, 4, 0x1), (0, 0, 0x0)
; CHECK-NOT:  .linkifunc

; CHECK:      {{^}}baseline:
; CHECK-NEXT: b baseline._Msimd
; CHECK-NOT:  .linkfunctionoverride
; CHECK-NOT:  .linkifunc

; CHECK:      {{^}}gated:
; CHECK-NEXT: b gated.default
; CHECK-NEXT: .linkfunctionoverride gated, (gated.cell, gated._Mlse), (3, 1, 0x10015), (3, 2, 0x10016), (2, 2, 0x0), (5, 4, 0x10007), (4, 4, 0x1), (0, 0, 0x0)
; CHECK-NEXT: {{^}}gated.cell:
; CHECK-NEXT: adrp x16, __imp_gated
; CHECK-NEXT: ldr x16, [x16, :lo12:__imp_gated]
; CHECK-NEXT: br x16
; CHECK-NEXT: .linkifunc gated, gated.resolver

; CHECK:      {{^}}cell:
; CHECK-NEXT: adrp x16, __imp_cell
; CHECK-NEXT: ldr x16, [x16, :lo12:__imp_cell]
; CHECK-NEXT: br x16
; CHECK-NEXT: .linkifunc cell, cell.resolver

$picked.resolver = comdat any
$baseline.resolver = comdat any
$gated.resolver = comdat any
$cell.resolver = comdat any

@picked = weak_odr dso_local ifunc i32 (), ptr @picked.resolver
@baseline = weak_odr dso_local ifunc i32 (), ptr @baseline.resolver
@gated = weak_odr dso_local ifunc i32 (), ptr @gated.resolver
@cell = weak_odr dso_local ifunc i32 (), ptr @cell.resolver

define weak_odr ptr @picked.resolver() comdat {
  %c1 = call i1 @sm4()
  %c2 = call i1 @lse()
  %s1 = select i1 %c2, ptr @picked._Mlse, ptr @picked.default
  %s2 = select i1 %c1, ptr @picked._Msm4, ptr %s1
  ret ptr %s2
}

define weak_odr ptr @baseline.resolver() comdat {
  ret ptr @baseline._Msimd
}

define weak_odr ptr @gated.resolver() comdat {
  %c1 = call i1 @sve()
  %c2 = call i1 @lse()
  %s1 = select i1 %c2, ptr @gated._Mlse, ptr @gated.default
  %s2 = select i1 %c1, ptr @gated._Msve, ptr %s1
  ret ptr %s2
}

define weak_odr ptr @cell.resolver() comdat {
  %c = call i1 @mops()
  %s = select i1 %c, ptr @cell._Mmops, ptr @cell.default
  ret ptr %s
}

define internal i32 @picked._Msm4() #0 { ret i32 1 }
define internal i32 @picked._Mlse() #1 { ret i32 2 }
define internal i32 @picked.default() #2 { ret i32 3 }
define internal i32 @baseline._Msimd() #3 { ret i32 4 }
define internal i32 @gated._Msve() #4 { ret i32 7 }
define internal i32 @gated._Mlse() #1 { ret i32 8 }
define internal i32 @gated.default() #2 { ret i32 9 }
define internal i32 @cell._Mmops() #5 { ret i32 5 }
define internal i32 @cell.default() #2 { ret i32 6 }

define i32 @use() {
  %a = call i32 @picked()
  %b = call i32 @baseline()
  %c = call i32 @gated()
  %d = call i32 @cell()
  %r1 = add i32 %a, %b
  %r2 = add i32 %r1, %c
  %r3 = add i32 %r2, %d
  ret i32 %r3
}

declare i1 @sm4()
declare i1 @lse()
declare i1 @sve()
declare i1 @mops()

attributes #0 = { "fmv-features"="sm4" "target-features"="+sm4" }
attributes #1 = { "fmv-features"="lse" "target-features"="+lse" }
attributes #2 = { "fmv-features" }
attributes #3 = { "fmv-features"="simd" "target-features"="+neon" }
attributes #4 = { "fmv-features"="sve" "target-features"="+sve" }
attributes #5 = { "fmv-features"="mops" "target-features"="+mops" }
