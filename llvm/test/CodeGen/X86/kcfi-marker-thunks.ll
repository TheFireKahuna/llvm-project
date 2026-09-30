; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj --symbols - | FileCheck %s --check-prefix=SYMS
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-readobj -r --symbols - | FileCheck %s --check-prefix=RANGE

;; With the kcfi-marker module flag, a call with a KCFI type goes through a
;; per-type thunk, a COMDAT that compares the 8 bytes before the target, the
;; end of the marker, the opcode of the move and the type, and on a mismatch
;; continues into a weak alias of the routine that fails fast if the target
;; carries the marker. On a match, a target inside the code range, whose bounds
;; are weak aliases of one byte in a COMDAT, is taken directly, and any other continues
;; into the guard function. The dispatch thunk takes the target in RAX and
;; jumps to it, and the check thunk takes it in RCX and returns.
;; The module has no cfguard flag, as under -mguard=none, and the thunks are
;; the same as with one. A call marked kcfi_local, whose every target is in
;; the image, goes through a local thunk, which fails fast for a target outside
;; the range unless the range is empty, as it is in an image that was not
;; sealed, where the guard function decides.

; CHECK-LABEL: f1:
; CHECK:         movq %rcx, %rax
; CHECK:         callq __llvm_kcfi_dispatch_12345678
define void @f1(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

; CHECK-LABEL: f2:
; CHECK:         movq %rcx, %rax
; CHECK:         jmp __llvm_kcfi_dispatch_12345678 # TAILCALL
define void @f2(ptr %p) {
  tail call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

; CHECK-LABEL: f3:
; CHECK:         movq %rdi, %rcx
; CHECK-NEXT:    callq __llvm_kcfi_check_00000010
; CHECK-NEXT:    xorl %eax, %eax
; CHECK-NEXT:    callq *%rdi
define x86_64_sysvcc void @f3(ptr %p) nounwind {
  call x86_64_sysvcc void (...) %p() [ "kcfi"(i32 16) ]
  ret void
}

; CHECK-LABEL: f4:
; CHECK:         callq __llvm_kcfi_local_dispatch_12345678
define void @f4(ptr %p) {
  call void %p() [ "kcfi"(i32 305419896) ], !kcfi_local !2
  ret void
}

; CHECK-LABEL: f5:
; CHECK:         callq __llvm_kcfi_local_check_00000010
define x86_64_sysvcc void @f5(ptr %p) nounwind {
  call x86_64_sysvcc void (...) %p() [ "kcfi"(i32 16) ], !kcfi_local !2
  ret void
}

; CHECK:       .weak __llvm_code_start
; CHECK-NEXT:  __llvm_code_start = __llvm_code_empty
; CHECK-NEXT:  .weak __llvm_code_end
; CHECK-NEXT:  __llvm_code_end = __llvm_code_empty
; CHECK-NEXT:  .section .rdata,"dr",discard,__llvm_code_empty
; CHECK-NEXT:  .globl __llvm_code_empty
; CHECK-NEXT:  __llvm_code_empty:
; CHECK-NEXT:  .byte 0
; CHECK-NEXT:  .weak __llvm_kcfi_mismatch_12345678
; CHECK-NEXT:  __llvm_kcfi_mismatch_12345678 = __llvm_kcfi_open
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_dispatch_12345678
; CHECK:       .globl __llvm_kcfi_dispatch_12345678
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_dispatch_12345678:
; CHECK-NEXT:    movabsq $1311768467969322430, %r11 # imm = 0x12345678B8DEADBE
; CHECK-NEXT:    cmpq %r11, -8(%rax)
; CHECK-NEXT:    jne __llvm_kcfi_mismatch_12345678
; CHECK-NEXT:    leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rax
; CHECK-NEXT:    jb [[GUARD:.Ltmp[0-9]+]]
; CHECK-NEXT:    leaq __llvm_code_end(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rax
; CHECK-NEXT:    jae [[GUARD]]
; CHECK-NEXT:    jmpq *%rax
; CHECK-NEXT:  [[GUARD]]:
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_open
; CHECK:       .globl __llvm_kcfi_open
; CHECK-NEXT:  .p2align 4
; CHECK-NEXT:  __llvm_kcfi_open:
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rax)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

; CHECK:       .weak __llvm_kcfi_check_mismatch_00000010
; CHECK-NEXT:  __llvm_kcfi_check_mismatch_00000010 = __llvm_kcfi_check_open
; CHECK-NEXT:  .section .text,"xr",discard,__llvm_kcfi_check_00000010
; CHECK:       __llvm_kcfi_check_00000010:
; CHECK-NEXT:    movabsq $71821077950, %r11 # imm = 0x10B8DEADBE
; CHECK-NEXT:    cmpq %r11, -8(%rcx)
; CHECK-NEXT:    jne __llvm_kcfi_check_mismatch_00000010
; CHECK-NEXT:    leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rcx
; CHECK-NEXT:    jb [[GUARD:.Ltmp[0-9]+]]
; CHECK-NEXT:    leaq __llvm_code_end(%rip), %r10
; CHECK-NEXT:    cmpq %r10, %rcx
; CHECK-NEXT:    jae [[GUARD]]
; CHECK-NEXT:    retq
; CHECK-NEXT:  [[GUARD]]:
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)
; CHECK:       __llvm_kcfi_check_open:
; CHECK-NEXT:    movabsq $-5125468290327503089, %r11 # imm = 0xB8DEADBEEF801F0F
; CHECK-NEXT:    cmpq %r11, -12(%rcx)
; CHECK-NEXT:    je [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41

;; The local thunks share the types' mismatch routines and open routines.
; CHECK:       .section .text,"xr",discard,__llvm_kcfi_local_dispatch_12345678
; CHECK:       __llvm_kcfi_local_dispatch_12345678:
; CHECK-NEXT:    movabsq $1311768467969322430, %r11 # imm = 0x12345678B8DEADBE
; CHECK-NEXT:    cmpq %r11, -8(%rax)
; CHECK-NEXT:    jne __llvm_kcfi_mismatch_12345678
; CHECK-NEXT:    leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:    leaq __llvm_code_end(%rip), %r11
; CHECK-NEXT:    cmpq %r10, %rax
; CHECK-NEXT:    jb [[GUARD:.Ltmp[0-9]+]]
; CHECK-NEXT:    cmpq %r11, %rax
; CHECK-NEXT:    jae [[GUARD]]
; CHECK-NEXT:    jmpq *%rax
; CHECK-NEXT:  [[GUARD]]:
; CHECK-NEXT:    cmpq %r11, %r10
; CHECK-NEXT:    jne [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41
; CHECK:       __llvm_kcfi_local_check_00000010:
; CHECK-NEXT:    movabsq $71821077950, %r11 # imm = 0x10B8DEADBE
; CHECK-NEXT:    cmpq %r11, -8(%rcx)
; CHECK-NEXT:    jne __llvm_kcfi_check_mismatch_00000010
; CHECK-NEXT:    leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:    leaq __llvm_code_end(%rip), %r11
; CHECK-NEXT:    cmpq %r10, %rcx
; CHECK-NEXT:    jb [[GUARD:.Ltmp[0-9]+]]
; CHECK-NEXT:    cmpq %r11, %rcx
; CHECK-NEXT:    jae [[GUARD]]
; CHECK-NEXT:    retq
; CHECK-NEXT:  [[GUARD]]:
; CHECK-NEXT:    cmpq %r11, %r10
; CHECK-NEXT:    jne [[TRAP:.Ltmp[0-9]+]]
; CHECK-NEXT:    jmpq *__guard_check_icall_fptr(%rip)
; CHECK-NEXT:  [[TRAP]]:
; CHECK-NEXT:    movl $64, %ecx
; CHECK-NEXT:    int $41
; CHECK-NOT:   __llvm_kcfi_mismatch_12345678 =
; CHECK-NOT:   __llvm_kcfi_open:
; CHECK-NOT:   __llvm_kcfi_check_open:

; SYMS:      Name: __llvm_kcfi_mismatch_12345678
; SYMS-NEXT: Value: 0
; SYMS-NEXT: Section: IMAGE_SYM_UNDEFINED (0)
; SYMS:      StorageClass: WeakExternal (0x69)
; SYMS:      Linked: __llvm_kcfi_open

;; The range bounds are weak externals whose default, __llvm_code_empty, is
;; defined in a COMDAT, so that references relocate against the bounds and
;; the range is empty unless the linker defines them.
; RANGE:      IMAGE_REL_AMD64_REL32 __llvm_code_start
; RANGE:      IMAGE_REL_AMD64_REL32 __llvm_code_end
; RANGE:      Name: __llvm_code_empty
; RANGE-NEXT: Value: 0
; RANGE-NEXT: Section: .rdata (
; RANGE:      StorageClass: External (0x2)
; RANGE:      Name: __llvm_code_start
; RANGE-NEXT: Value: 0
; RANGE-NEXT: Section: IMAGE_SYM_UNDEFINED (0)
; RANGE:      StorageClass: WeakExternal (0x69)
; RANGE-NEXT: AuxSymbolCount: 1
; RANGE-NEXT: AuxWeakExternal {
; RANGE-NEXT:   Linked: __llvm_code_empty
; RANGE-NEXT:   Search: Alias
; RANGE:      Name: __llvm_code_end
; RANGE-NEXT: Value: 0
; RANGE-NEXT: Section: IMAGE_SYM_UNDEFINED (0)
; RANGE:      StorageClass: WeakExternal (0x69)
; RANGE-NEXT: AuxSymbolCount: 1
; RANGE-NEXT: AuxWeakExternal {
; RANGE-NEXT:   Linked: __llvm_code_empty
; RANGE-NEXT:   Search: Alias

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!2 = !{}
