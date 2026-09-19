# REQUIRES: x86

# With /guard:cf the guard tables carry a flag byte per entry. A function that
# is a valid indirect-call target only because it is exported is marked
# export-suppressed when its entry is 16-byte aligned; an unaligned one, and
# one whose address is also taken, are ordinary entries. The image declares
# that the information is complete, and an executable can enable the mode.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/dll.s -filetype=obj -o %t.dll.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/loadcfg.s -filetype=obj -o %t.loadcfg.obj
# RUN: lld-link %t.dll.obj %t.loadcfg.obj -guard:cf -dll -noentry -out:%t.dll
# RUN: llvm-readobj --coff-load-config %t.dll | FileCheck %s --check-prefix=DLL

# DLL:      GuardFlags [ (0x10014500)
# DLL-NEXT:   CF_EXPORT_SUPPRESSION_INFO_PRESENT (0x4000)
# DLL-NEXT:   CF_FUNCTION_TABLE_PRESENT (0x400)
# DLL-NEXT:   CF_FUNCTION_TABLE_SIZE_5BYTES (0x10000000)
# DLL-NEXT:   CF_INSTRUMENTED (0x100)
# DLL-NEXT:   CF_LONGJUMP_TABLE_PRESENT (0x10000)
# DLL-NEXT: ]
# DLL:      GuardFidTable [
# DLL-NEXT:   0x180001000 flags 2
# DLL-NEXT:   0x180001010{{$}}
# DLL-NEXT:   0x180001020{{$}}
# DLL-NEXT:   0x180001031{{$}}
# DLL-NEXT: ]

# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/exe.s -filetype=obj -o %t.exe.obj
# RUN: lld-link %t.exe.obj -guard:cf -entry:main -out:%t.exe
# RUN: llvm-readobj --coff-load-config %t.exe | FileCheck %s --check-prefix=EXE
# RUN: lld-link %t.exe.obj -guard:cf,exportsuppress -entry:main -out:%t.es.exe
# RUN: llvm-readobj --coff-load-config %t.es.exe | FileCheck %s --check-prefix=ES
# RUN: lld-link %t.exe.obj -guard:cf,exportsuppress,noexportsuppress -entry:main -out:%t.no.exe
# RUN: llvm-readobj --coff-load-config %t.no.exe | FileCheck %s --check-prefix=EXE

# EXE:      GuardFlags [ (0x10014500)
# EXE:      GuardFidTable [
# EXE-NEXT:   0x140001000{{$}}
# EXE-NEXT: ]

# ES:       GuardFlags [ (0x1001C500)
# ES-NEXT:    CF_ENABLE_EXPORT_SUPPRESSION (0x8000)
# ES-NEXT:    CF_EXPORT_SUPPRESSION_INFO_PRESENT (0x4000)

## A load configuration too short to hold the address-taken IAT table cannot
## declare the information present.
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/short.s -filetype=obj -o %t.short.obj
# RUN: lld-link %t.dll.obj %t.short.obj -guard:cf -dll -noentry -out:%t.short.dll
# RUN: llvm-readobj --coff-load-config %t.short.dll | FileCheck %s --check-prefix=SHORT

# SHORT:      GuardFlags [ (0x10010500)
# SHORT-NEXT:   CF_FUNCTION_TABLE_PRESENT (0x400)
# SHORT-NEXT:   CF_FUNCTION_TABLE_SIZE_5BYTES (0x10000000)
# SHORT-NEXT:   CF_INSTRUMENTED (0x100)
# SHORT-NEXT:   CF_LONGJUMP_TABLE_PRESENT (0x10000)
# SHORT-NEXT: ]

#--- dll.s
        .text
        .globl @feat.00
.set @feat.00, 2048

        .def export_only; .scl 2; .type 32; .endef
        .globl export_only
        .p2align 4
export_only:
        ret

        .def export_taken; .scl 2; .type 32; .endef
        .globl export_taken
        .p2align 4
export_taken:
        ret

        .def local_taken; .scl 2; .type 32; .endef
        .globl local_taken
        .p2align 4
local_taken:
        ret

        .def unaligned; .scl 2; .type 32; .endef
        .globl unaligned
        .p2align 4
        nop
unaligned:
        ret

        .section .gfids$y,"dr"
        .symidx export_taken
        .symidx local_taken

        .section .drectve,"dr"
        .ascii " /EXPORT:export_only /EXPORT:export_taken /EXPORT:unaligned"

#--- loadcfg.s
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

#--- exe.s
        .text
        .globl @feat.00
.set @feat.00, 2048
        .def main; .scl 2; .type 32; .endef
        .globl main
        .p2align 4
main:
        xorl %eax, %eax
        ret

        .section .gfids$y,"dr"

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

#--- short.s
        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 148
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
