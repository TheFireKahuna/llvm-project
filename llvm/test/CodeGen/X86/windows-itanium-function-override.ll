; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s

;; A multiversioned function whose every version needs only features Windows
;; can test as it maps the image is a stub that jumps to the version every
;; supported processor runs, with a record of the versions Windows may select
;; instead: tests of each version's capabilities in priority order, each
;; ending in a leaf that selects it, then a terminal that keeps the default.
;; A call is direct. One whose versions all lie within the baseline is just
;; the stub. One needing a feature Windows cannot test keeps its cell.

; CHECK-LABEL: use:
; CHECK:         callq picked
; CHECK:         callq baseline
; CHECK:         callq *__imp_cell(%rip)

; CHECK:      .p2align 4
; CHECK-NEXT: {{^}}picked:
; CHECK-NEXT: jmp picked.default
; CHECK-NEXT: .linkfunctionoverride picked, (picked.fsrm, picked.avx512f), (2, 1, 0x1), (1, 1, 0x0), (4, 3, 0x13e), (3, 3, 0x1), (0, 0, 0x0)
; CHECK-NOT:  .linkifunc

; CHECK:      {{^}}baseline:
; CHECK-NEXT: jmp baseline.avx2
; CHECK-NOT:  .linkfunctionoverride
; CHECK-NOT:  .linkifunc

; CHECK:      {{^}}cell:
; CHECK-NEXT: jmpq *__imp_cell(%rip)
; CHECK-NEXT: .linkifunc cell, cell.resolver

$picked.resolver = comdat any
$baseline.resolver = comdat any
$cell.resolver = comdat any

@picked = weak_odr dso_local ifunc i32 (), ptr @picked.resolver
@baseline = weak_odr dso_local ifunc i32 (), ptr @baseline.resolver
@cell = weak_odr dso_local ifunc i32 (), ptr @cell.resolver

define weak_odr ptr @picked.resolver() comdat {
  %c1 = call i1 @fsrm()
  %c2 = call i1 @avx512f()
  %s1 = select i1 %c2, ptr @picked.avx512f, ptr @picked.default
  %s2 = select i1 %c1, ptr @picked.fsrm, ptr %s1
  ret ptr %s2
}

define weak_odr ptr @baseline.resolver() comdat {
  ret ptr @baseline.avx2
}

define weak_odr ptr @cell.resolver() comdat {
  %c = call i1 @avx512f()
  %s = select i1 %c, ptr @cell.vbmi, ptr @cell.default
  ret ptr %s
}

define internal i32 @picked.fsrm() #0 { ret i32 1 }
define internal i32 @picked.avx512f() #1 { ret i32 2 }
define internal i32 @picked.default() #2 { ret i32 3 }
define internal i32 @baseline.avx2() #3 { ret i32 4 }
define internal i32 @cell.vbmi() #4 { ret i32 5 }
define internal i32 @cell.default() #2 { ret i32 6 }

define i32 @use() {
  %a = call i32 @picked()
  %b = call i32 @baseline()
  %c = call i32 @cell()
  %r1 = add i32 %a, %b
  %r2 = add i32 %r1, %c
  ret i32 %r2
}

declare i1 @fsrm()
declare i1 @avx512f()

attributes #0 = { "fmv-features"="fsrm" "target-features"="+fsrm" }
attributes #1 = { "fmv-features"="avx512f" "target-features"="+avx512f" }
attributes #2 = { "fmv-features" }
attributes #3 = { "fmv-features"="avx2" "target-features"="+avx2" }
attributes #4 = { "fmv-features"="avx512vbmi" "target-features"="+avx512vbmi" }
