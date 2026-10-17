# REQUIRES: x86

## Under -guard:cf, the dynamic scanner that the bounded
## routine of a closed type jumps to, which nothing else refers to, is kept
## before the garbage collector runs, so that it keeps its unwind information
## and its place in its section.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: lld-link main.obj plain.obj -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-readobj --unwind main.exe | FileCheck %s --check-prefix=UNWIND

# CHECK:      <__llvm_kcfi_open_dynamic>:
# CHECK:      <plain>:
# CHECK:      <__llvm_kcfi_mismatch_22222222>:
# CHECK:        jmp 0x140001040 <__llvm_kcfi_open_dynamic>

# UNWIND: StartAddress: __llvm_kcfi_open_dynamic (0x140001040)

#--- plain.s
        .def plain; .scl 2; .type 32; .endef
        .text
        .globl plain
plain:
        retq

        .data
        .quad plain

#--- main.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .fill 4, 1, 0x90
__cfi_main:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl main
main:
        callq __llvm_kcfi_dispatch_22222222
        retq

        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_22222222
        .globl __llvm_kcfi_dispatch_22222222
        .p2align 4
__llvm_kcfi_dispatch_22222222:
        cmpl $0x22222222, -4(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .section .text,"xr",discard,__llvm_kcfi_open_dynamic
        .globl __llvm_kcfi_open_dynamic
        .p2align 4
        .def __llvm_kcfi_open_dynamic; .scl 2; .type 32; .endef
        .seh_proc __llvm_kcfi_open_dynamic
__llvm_kcfi_open_dynamic:
        pushq %rbx
        .seh_pushreg %rbx
        .seh_endprologue
        popq %rbx
        ud2
        .seh_endproc

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0
