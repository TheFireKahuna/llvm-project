# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-pc-win32 %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf,exportsuppress -dll -out:%t.dll -noentry \
# RUN:   -guardsym:other_func,S -guardsym:nosuch,S 2>&1 | count 0
# RUN: llvm-readobj --coff-load-config %t.dll | FileCheck %s --check-prefix=CHECK

# There should be an entry in the table for the exported symbol, which is
# export-suppressed, and one for the address-taken local function the
# /GUARDSYM directive suppresses. /guardsym adds no function to the table, and
# names no symbol silently. Export suppression is enabled only in an
# executable.
#
# CHECK-NOT: CF_ENABLE_EXPORT_SUPPRESSION
# CHECK: GuardFidTable [
# CHECK-NEXT: 0x180001000 flags 1
# CHECK-NEXT: 0x180001010 flags 2
# CHECK-NEXT: ]

# RUN: lld-link %t.obj -guard:cf,exportsuppress -entry:func_export -subsystem:console -out:%t.exe \
# RUN:   -guardsym:func_export,X 2>&1 | FileCheck %s --check-prefix=WARN
# RUN: llvm-readobj --coff-load-config %t.exe | FileCheck %s --check-prefix=EXE

# WARN: warning: ignoring /guardsym:func_export,X: its only flag is S
# EXE: CF_ENABLE_EXPORT_SUPPRESSION (0x8000)

        .text
        .def    local_func; .scl    3; .type   32; .endef
local_func:
        ret
        .globl other_func
        .def    other_func; .scl    2; .type   32; .endef
other_func:
        ret

        .def    func_export; .scl    2; .type   32; .endef
        .globl func_export
        .section       .text,"xr",one_only,func_export
        .p2align 4
func_export:
        movl $1, %eax
        .globl label_export
label_export:
        movl $2, %eax
        ret

        .data
        .globl data_export
data_export:
        .long 42
        .quad local_func

        .section .drectve,"dr"
        .ascii " /EXPORT:func_export"
        .ascii " /EXPORT:label_export"
        .ascii " /EXPORT:data_export"
        .ascii " /GUARDSYM:local_func,S"


# Load configuration directory entry (winnt.h _IMAGE_LOAD_CONFIG_DIRECTORY64).
# The linker will define the __guard_* symbols.
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
