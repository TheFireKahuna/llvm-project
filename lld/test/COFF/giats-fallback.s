# REQUIRES: x86

# An object without guard metadata gives no address-taken IAT table of its
# own. A reference that reads an import pointer other than to call or jump
# through it hands the imported address on, so the linker lists that entry;
# a call or jump through the pointer is not listed.

# RUN: yaml2obj %p/Inputs/export.yaml -o %t.exp.obj
# RUN: lld-link -out:%t.exp.dll -dll %t.exp.obj -export:exportfn1 -export:exportfn2 -implib:%t.exp.lib

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj %t.exp.lib -guard:cf -entry:main -out:%t.exe
# RUN: llvm-readobj --coff-imports --coff-load-config %t.exe | FileCheck %s

# CHECK:      Import {
# CHECK-NEXT:   Name: giats-fallback.s.tmp.exp.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[IAT:[0-9A-F]+]]
# CHECK-NEXT:   Symbol: exportfn1
# CHECK-NEXT:   Symbol: exportfn2
# CHECK:      GuardAddressTakenIatEntryCount: 1
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x14000[[IAT]]
# CHECK-NEXT: ]

#--- main.s
        .text
        .def main; .scl 2; .type 32; .endef
        .globl main
        .p2align 4
main:
        movq __imp_exportfn1(%rip), %rax
        callq *__imp_exportfn2(%rip)
        jmpq *%rax

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .fill 104, 1, 0
