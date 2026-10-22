; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; A multiversioned function whose every version needs only features Windows
;; can test as it maps the image is a stub that branches to the version every
;; supported processor runs, with a record of the versions Windows may select
;; instead: tests of each version's capabilities, with those its features
;; imply, in priority order, each ending in a leaf that selects it, then a
;; terminal that keeps the default. A call is direct. One whose versions all
;; lie within the baseline is just the stub. One needing a feature Windows
;; cannot test keeps its cell.

; CHECK-LABEL: use:
; CHECK:         bl picked
; CHECK:         bl baseline
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

; CHECK:      {{^}}cell:
; CHECK-NEXT: adrp x16, __imp_cell
; CHECK-NEXT: ldr x16, [x16, :lo12:__imp_cell]
; CHECK-NEXT: br x16
; CHECK-NEXT: .linkifunc cell, cell.resolver

$picked.resolver = comdat any
$baseline.resolver = comdat any
$cell.resolver = comdat any

@picked = weak_odr dso_local ifunc i32 (), ptr @picked.resolver
@baseline = weak_odr dso_local ifunc i32 (), ptr @baseline.resolver
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

define weak_odr ptr @cell.resolver() comdat {
  %c = call i1 @sve()
  %s = select i1 %c, ptr @cell._Msve, ptr @cell.default
  ret ptr %s
}

define internal i32 @picked._Msm4() #0 { ret i32 1 }
define internal i32 @picked._Mlse() #1 { ret i32 2 }
define internal i32 @picked.default() #2 { ret i32 3 }
define internal i32 @baseline._Msimd() #3 { ret i32 4 }
define internal i32 @cell._Msve() #4 { ret i32 5 }
define internal i32 @cell.default() #2 { ret i32 6 }

define i32 @use() {
  %a = call i32 @picked()
  %b = call i32 @baseline()
  %c = call i32 @cell()
  %r1 = add i32 %a, %b
  %r2 = add i32 %r1, %c
  ret i32 %r2
}

declare i1 @sm4()
declare i1 @lse()
declare i1 @sve()

attributes #0 = { "fmv-features"="sm4" "target-features"="+sm4" }
attributes #1 = { "fmv-features"="lse" "target-features"="+lse" }
attributes #2 = { "fmv-features" }
attributes #3 = { "fmv-features"="simd" "target-features"="+neon" }
attributes #4 = { "fmv-features"="sve" "target-features"="+sve" }
