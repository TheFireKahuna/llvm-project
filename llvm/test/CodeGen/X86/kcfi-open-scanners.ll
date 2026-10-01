; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=BYTES

;; An object with a KCFI thunk emits the trap and the scanners of the thunk's
;; kind, each in a COMDAT, for open routines of any object to use. A scanner
;; takes the target in the thunk's register and the type's list in R10. It
;; fails fast if the target carries the marker, which it reads only at page
;; offset 16 or more, and then walks the list: a zero word is skipped, an odd
;; word ends it, and any other word is the address of a cell, whose value, if
;; it is the target, is taken. On reaching the end, the static scanner fails
;; fast and the dynamic one continues into the guard function.

; CHECK-LABEL: __llvm_kcfi_open:
; CHECK-NEXT:    testl $4080, %eax
; CHECK-NEXT:    je [[WALK:.Ltmp[0-9]+]]
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rax)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[WALK]]:
; CHECK-NEXT:    movq (%r10), %r11
; CHECK-NEXT:    addq $8, %r10
; CHECK-NEXT:    testq %r11, %r11
; CHECK-NEXT:    je [[WALK]]
; CHECK-NEXT:    testb $1, %r11b
; CHECK-NEXT:    jne [[TRAP]]
; CHECK-NEXT:    cmpq (%r11), %rax
; CHECK-NEXT:    jne [[WALK]]
; CHECK-NEXT:    jmpq *%rax
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; CHECK-LABEL: __llvm_kcfi_open_dynamic:
; CHECK-NEXT:    testl $4080, %eax
; CHECK-NEXT:    je [[WALK:.Ltmp[0-9]+]]
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rax)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[WALK]]:
; CHECK-NEXT:    movq (%r10), %r11
; CHECK-NEXT:    addq $8, %r10
; CHECK-NEXT:    testq %r11, %r11
; CHECK-NEXT:    je [[WALK]]
; CHECK-NEXT:    testb $1, %r11b
; CHECK-NEXT:    jne [[MISS:.Ltmp[0-9]+]]
; CHECK-NEXT:    cmpq (%r11), %rax
; CHECK-NEXT:    jne [[WALK]]
; CHECK-NEXT:    jmpq *%rax
; CHECK-NEXT:  [[MISS]]:
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; CHECK-LABEL: __llvm_kcfi_check_open:
; CHECK-NEXT:    testl $4080, %ecx
; CHECK-NEXT:    je [[WALK:.Ltmp[0-9]+]]
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rcx)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[WALK]]:
; CHECK-NEXT:    movq (%r10), %r11
; CHECK-NEXT:    addq $8, %r10
; CHECK-NEXT:    testq %r11, %r11
; CHECK-NEXT:    je [[WALK]]
; CHECK-NEXT:    testb $1, %r11b
; CHECK-NEXT:    jne [[TRAP]]
; CHECK-NEXT:    cmpq (%r11), %rcx
; CHECK-NEXT:    jne [[WALK]]
; CHECK-NEXT:    retq
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; CHECK-LABEL: __llvm_kcfi_check_open_dynamic:
; CHECK-NEXT:    testl $4080, %ecx
; CHECK-NEXT:    je [[WALK:.Ltmp[0-9]+]]
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rcx)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[WALK]]:
; CHECK-NEXT:    movq (%r10), %r11
; CHECK-NEXT:    addq $8, %r10
; CHECK-NEXT:    testq %r11, %r11
; CHECK-NEXT:    je [[WALK]]
; CHECK-NEXT:    testb $1, %r11b
; CHECK-NEXT:    jne [[MISS:.Ltmp[0-9]+]]
; CHECK-NEXT:    cmpq (%r11), %rcx
; CHECK-NEXT:    jne [[WALK]]
; CHECK-NEXT:    retq
; CHECK-NEXT:  [[MISS]]:
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; CHECK:       .section .text,"xr",discard,__llvm_kcfi_trap
; CHECK-LABEL: __llvm_kcfi_trap:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; BYTES-LABEL: <__llvm_kcfi_open>:
; BYTES-NEXT:    0: a9 f0 0f 00 00                testl $0xff0, %eax
; BYTES-NEXT:    5: 74 10                         je 0x17
; BYTES-NEXT:    7: 49 bb 0f 1f 80 ef be ad de b8 movabsq $-0x47215241107fe0f1, %r11
; BYTES-NEXT:   11: 4c 39 58 f4                   cmpq %r11, -0xc(%rax)
; BYTES-NEXT:   15: 74 19                         je 0x30
; BYTES-NEXT:   17: 4d 8b 1a                      movq (%r10), %r11
; BYTES-NEXT:   1a: 49 83 c2 08                   addq $0x8, %r10
; BYTES-NEXT:   1e: 4d 85 db                      testq %r11, %r11
; BYTES-NEXT:   21: 74 f4                         je 0x17
; BYTES-NEXT:   23: 41 f6 c3 01                   testb $0x1, %r11b
; BYTES-NEXT:   27: 75 07                         jne 0x30
; BYTES-NEXT:   29: 49 3b 03                      cmpq (%r11), %rax
; BYTES-NEXT:   2c: 75 e9                         jne 0x17
; BYTES-NEXT:   2e: ff e0                         jmpq *%rax
; BYTES-NEXT:   30: b9 40 00 00 00                movl $0x40, %ecx
; BYTES-NEXT:   35: cd 29                         int $0x29
; BYTES-EMPTY:
; BYTES-LABEL: <__llvm_kcfi_trap>:
; BYTES-NEXT:    0: b9 40 00 00 00                movl $0x40, %ecx
; BYTES-NEXT:    5: cd 29                         int $0x29
; BYTES-EMPTY:

define void @f1(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

define x86_64_sysvcc void @f2(ptr %p) nounwind {
  call x86_64_sysvcc void (...) %p() [ "kcfi"(i32 16) ]
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
