# REQUIRES: x86

# With /guard:ehcont, SEH in a COMDAT of an object without EH continuation
# metadata is a warning, as link.exe's LNK4291 is, and the table lists the
# __except blocks of its scope tables, which are where __C_specific_handler
# can continue. A __finally scope has no target. SEH outside a COMDAT stays an
# error (guard-ehcont-missing.s).

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -guard:ehcont -out:%t.exe -entry:main \
# RUN:   2>&1 | FileCheck %s --check-prefix=WARN
# RUN: llvm-readobj --coff-load-config %t.exe | FileCheck %s
# RUN: llvm-objdump -d %t.exe | FileCheck %s --check-prefix=DIS

# WARN: warning: /guard:ehcont: {{.*}}.obj has no EH continuation metadata; its SEH scope tables' __except blocks are listed instead

# CHECK:      GuardEHContinuationCount: 1
# CHECK:      GuardEHContTable [
# CHECK-NEXT:   0x14000101A
# CHECK-NEXT: ]
# DIS:      14000101a: 48 83 c4 28 addq $0x28, %rsp

        .section .text,"xr",one_only,main
        .globl main
        .def main; .scl 2; .type 32; .endef
        .seh_proc main
main:
        .seh_handler __C_specific_handler, @except
        subq $40, %rsp
        .seh_stackalloc 40
        .seh_endprologue
.Lbegin:
        nop
.Lend:
        addq $40, %rsp
        retq
.Lexcept:
        addq $40, %rsp
        retq
        .seh_handlerdata
        .long 2
        .long .Lbegin@IMGREL
        .long .Lend@IMGREL
        .long 1
        .long .Lexcept@IMGREL
        .long .Lbegin@IMGREL
        .long .Lend@IMGREL
        .long main@IMGREL
        .long 0
        .section .text,"xr",one_only,main
        .seh_endproc

        .text
        .globl __C_specific_handler
__C_specific_handler:
        retq

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 312
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .quad __guard_longjmp_table
        .quad __guard_longjmp_count
        .fill 72, 1, 0
        .quad __guard_eh_cont_table
        .quad __guard_eh_cont_count
        .fill 32, 1, 0
