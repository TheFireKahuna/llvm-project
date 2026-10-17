; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=OBJ

;; With the function-type-prefix module flag, every typed function gets a prefix
;; whose 8 bytes before the type are 0F 1F 80, the marker, and B8, whether or
;; not the module has the kcfi flag. The marker is always a 32-bit displacement.
;; The __cfi_ symbol follows the padding, at the first type word.

; ASM:       .p2align 4
; ASM-COUNT-4: nop
; ASM-NEXT:  __cfi_f1:
; ASM-NEXT:    {disp32} nopl 16(%rax)
; ASM-NEXT:    movl $12345678, %eax
; ASM-LABEL: {{^}}f1:

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
define void @f1() !kcfi_type !1 {
  ret void
}

;; An untyped function has no prefix.
; ASM-NOT:   __cfi_f2:
; ASM:       .p2align 4
; ASM-NOT:   nop
; ASM-LABEL: f2:
; OBJ-NOT:  <__cfi_f2>:
; OBJ:      <f2>:
define void @f2() {
  ret void
}

;; A function with a second type carries it 16 bytes before its entry.
; ASM:       .p2align 4
; ASM-LABEL: __cfi_f3:
; ASM-NEXT:    .long 305419896
; ASM-NEXT:    {disp32} nopl 16(%rax)
; ASM-NEXT:    movl $12345678, %eax
; ASM-LABEL: f3:

; OBJ:      <__cfi_f3>:
; OBJ-NEXT:   78 56
; OBJ-NEXT:   34 12
; OBJ-NEXT:   0f 1f 80 10 00 00 00 nopl 0x10(%rax)
; OBJ-NEXT:   b8 4e 61 bc 00       movl $0xbc614e, %eax
; OBJ-EMPTY:
; OBJ-NEXT: <f3>:
define void @f3() !kcfi_type !1 !kcfi_vfn_type !2 {
  ret void
}

;; A second type that would spell ENDBR64 is stored plus one, as the type is.
; ASM-LABEL: __cfi_f4:
; ASM-NEXT:    .long 4196274164
; OBJ:      <__cfi_f4>:
; OBJ-NEXT:   f4 hlt
; OBJ-NEXT:   0f 1e
; OBJ-NEXT:   fa cli
define void @f4() !kcfi_type !1 !kcfi_vfn_type !3 {
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"function-type-prefix", i32 16}
!1 = !{i32 12345678}
!2 = !{i32 305419896}
!3 = !{i32 -98693133}
