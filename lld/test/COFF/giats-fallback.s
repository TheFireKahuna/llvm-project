# REQUIRES: x86

# An object without guard metadata gives no address-taken IAT table of its
# own, so every import address table entry it references is listed: without
# metadata a call through the entry cannot be told from a read of it.

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
# CHECK:      GuardAddressTakenIatEntryCount: 2
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x14000[[IAT]]
# CHECK-NEXT:   0x14000{{[0-9A-F]+}}
# CHECK-NEXT: ]

# A delay-loaded entry holds its load thunk until it is resolved, so the thunk
# is a valid call target too.
# RUN: lld-link %t.obj %t.exp.lib -guard:cf -entry:main -out:%t.delay.exe \
# RUN:   -delayload:giats-fallback.s.tmp.exp.dll \
# RUN:   -alternatename:__delayLoadHelper2=main
# RUN: llvm-readobj --coff-load-config %t.delay.exe | FileCheck %s --check-prefix=DELAY

# DELAY:      GuardFidTable [
# DELAY-NEXT:   0x140001000
# DELAY-NEXT:   0x140001010
# DELAY-NEXT:   0x140001020
# DELAY-NEXT: ]
# DELAY-NEXT: GuardIatTable [
# DELAY-NEXT:   0x140003008
# DELAY-NEXT:   0x140003010
# DELAY-NEXT: ]

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
