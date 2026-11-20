; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=OBJ
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=A64 %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-ntposix -filetype=obj < %s \
; RUN:   | llvm-objdump -s -j .text - | FileCheck %s --check-prefix=A64OBJ %}

;; With the function-type-prefix module flag, the 8 bytes before a function's
;; type are 0F 1F 80, the marker, and B8: a nopl whose displacement is the
;; marker, always 32 bits wide, and the opcode of the move that holds the type.
;; AArch64 emits the same bytes as data. A local __cfi_ symbol follows the
;; padding, at the prefix's start, and the nops align the entry to 16 bytes.

; OBJ:        90 nop
; OBJ-NEXT:   90 nop
; OBJ-NEXT:   90 nop
; OBJ-NEXT:   90 nop
; OBJ-EMPTY:
; OBJ-NEXT: <__cfi_f1>:
; OBJ-NEXT:   0f 1f 80 10 00 00 00 nopl 0x10(%rax)
; OBJ-NEXT:   b8 4e 61 bc 00       movl $0xbc614e, %eax
; OBJ-EMPTY:
; OBJ-NEXT: <f1>:
; A64-NOT:   .globl __cfi_
; A64:       nop
; A64-NEXT:  __cfi_f1:
define void @f1() !kcfi_type !2 {
  ret void
}

;; An untyped function has no prefix.
; ASM-LABEL: {{^}}f1:
; ASM-NOT:   nop
; ASM-LABEL: {{^}}f2:
define void @f2() {
  ret void
}

;; The nops of a patchable prefix follow the type and count toward the
;; alignment.
; ASM:       .p2align 4
; ASM-COUNT-3: nop
; ASM-NEXT:  __cfi_f3:
; ASM-NEXT:    {disp32} nopl 16(%rax)
; ASM-NEXT:    movl $12345678, %eax
; ASM-NEXT:  .Ltmp{{.*}}:
; ASM-NEXT:    nop
; ASM-NEXT:  {{^}}f3:
define void @f3() "patchable-function-prefix"="1" !kcfi_type !2 {
  ret void
}

;; A function with a second type carries it before the marker, 16 bytes before
;; its entry, and its nopl says so by its base register, RCX: a check of a
;; second type compares the two as one, and so fails on a prefix without one.
;; On x86-64 one that would spell ENDBR64 is stored plus one, as the type is.
; ASM:       .p2align 4
; ASM-NEXT:  __cfi_f4:
; ASM-NEXT:    .long 7
; ASM-NEXT:    {disp32} nopl 16(%rcx)
; ASM:       __cfi_f5:
; ASM-NEXT:    .long 4196274164
define void @f4() !kcfi_type !2 !kcfi_vfn_type !3 {
  ret void
}

define void @f5() !kcfi_type !2 !kcfi_vfn_type !4 {
  ret void
}

; A64OBJ:      0000 1f2003d5 0f1f8010 000000b8 4e61bc00
; A64OBJ-NEXT: 0010 c0035fd6 c0035fd6 1f2003d5 1f2003d5
; A64OBJ-NEXT: 0020 0f1f8010 000000b8 4e61bc00 1f2003d5
; A64OBJ-NEXT: 0030 c0035fd6 1f2003d5 1f2003d5 1f2003d5
; A64OBJ-NEXT: 0040 07000000 0f1f8110 000000b8 4e61bc00
; A64OBJ-NEXT: 0050 c0035fd6 1f2003d5 1f2003d5 1f2003d5
; A64OBJ-NEXT: 0060 f30f1efa 0f1f8110 000000b8 4e61bc00

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 7}
!4 = !{i32 -98693133}
