; RUN: llc -mtriple=x86_64-unknown-windows-itanium -data-sections < %s | \
; RUN:   FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -data-sections \
; RUN:   -filetype=obj < %s | llvm-objdump -s -j .llvm_link_records - | \
; RUN:   FileCheck --check-prefix=OBJ %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | \
; RUN:   FileCheck --check-prefix=SHARED %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu < %s | \
; RUN:   FileCheck --check-prefix=ELF %s

; A pin asks for the address at its offset into the global; the global's own
; symbol is pinned, with the residue moved back by the offset.
; CHECK-LABEL: _ZTV1D:
; CHECK:       .linkpin _ZTV1D, 12, 4072, required
@_ZTV1D = constant [5 x ptr] zeroinitializer, align 8, !pin !0

; A pin that is not required, on a global aligned to 8.
; CHECK-LABEL: _ZTV1A:
; CHECK:       .linkpin _ZTV1A, 6, 56{{$}}
@_ZTV1A = constant [6 x ptr] zeroinitializer, align 8, !pin !1

; A pin that is not required gives way to an alignment it disagrees with.
; CHECK-LABEL: _ZTV1B:
; CHECK-NOT:   .linkpin
@_ZTV1B = constant [6 x ptr] zeroinitializer, align 64, !pin !1

; A pin that is not required is dropped when the global shares its section,
; which the linker places as a whole; a required one stays.
; SHARED:     .linkpin _ZTV1D, 12, 4072, required
; SHARED-NOT: .linkpin

; ELF-NOT: .linkpin

; OBJ: Contents of section .llvm_link_records:

!0 = !{i64 16, i64 12, i64 4088, i64 1}
!1 = !{i64 0, i64 6, i64 56, i64 0}
