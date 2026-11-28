; REQUIRES: x86, aarch64
; RUN: rm -rf %t && split-file %s %t
; DEFINE: %{triple} = x86_64-unknown-windows-itanium
; DEFINE: %{link} = llc -mtriple=%{triple} -filetype=obj %t/use.ll -o %t/use.obj && \
; DEFINE:   llc -mtriple=%{triple} -filetype=obj %t/def.ll -o %t/def.obj && \
; DEFINE:   lld-link -dll -noentry -out:%t/use.dll %t/use.obj %t/def.obj \
; DEFINE:     -export:call -export:tail -export:address -export:called \
; DEFINE:     -export:addressed && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn %t/use.dll
; DEFINE: %{guard} = llvm-mc -triple=%{triple} -filetype=obj %t/config.s \
; DEFINE:     -o %t/config.obj && \
; DEFINE:   lld-link -dll -noentry -guard:cf -out:%t/guard.dll %t/def.obj \
; DEFINE:     %t/use.obj %t/config.obj -export:called -export:addressed && \
; DEFINE:   llvm-readobj --coff-load-config %t/guard.dll
; RUN: %{link} | FileCheck %s --check-prefix=X64
; RUN: %{guard} | FileCheck %s --check-prefix=GUARD
; REDEFINE: %{triple} = aarch64-pc-windows-ntposix
; RUN: %{link} | FileCheck %s --check-prefix=A64
; RUN: %{guard} | FileCheck %s --check-prefix=GUARD

; The compiler reaches a function that the module only declares through its
; import pointer. Where the function turns out to be in the image, the linker
; makes the call and the tail call direct and the load of the address the
; address itself, so that no instruction reads a pointer. Each of two calls
; reads the pointer for itself, so both are direct and no register is saved.

; X64-LABEL: <call>:
; X64-NEXT:    subq
; X64-NEXT:    callq {{.*}} <called>
; X64-NEXT:    callq {{.*}} <called>
; X64-LABEL: <tail>:
; X64-NEXT:    jmp {{.*}} <called>
; X64-LABEL: <address>:
; X64-NEXT:    leaq {{.*}} <addressed>

; A64-LABEL: <address>:
; A64-NEXT:    adrp x0,
; A64-NEXT:    add x0, x0,

; The function whose address the code takes is a call target of the image,
; though no pointer to it remains, and the one it only calls is not: as an
; export it is listed, and suppressed.

; GUARD:      GuardFidTable [
; GUARD-NEXT:   0x180001000 flags 2
; GUARD-NEXT:   0x180001010{{$}}
; GUARD-NEXT: ]

;--- use.ll
declare void @called() nonlazybind
declare void @addressed()

define void @call() {
  call void @called()
  call void @called()
  ret void
}

define void @tail() {
  tail call void @called()
  ret void
}

define ptr @address() {
  ret ptr @addressed
}

;--- def.ll
define void @called() align 16 {
  ret void
}

define void @addressed() align 16 {
  ret void
}

;--- config.s
.section .rdata,"dr"
.globl _load_config_used
.p2align 3
_load_config_used:
  .long 0x140
  .fill 0x7c, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 0xac, 1, 0
