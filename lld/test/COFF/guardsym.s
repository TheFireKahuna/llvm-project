# REQUIRES: x86

# /guardsym:<symbol>,S, given in a .drectve section as the UCRT's objects give
# it or on the command line, keeps the function the symbol names in the table
# of call targets but marks it suppressed, which takes a flag byte per entry.
# An exported function can be both export-suppressed and suppressed. A symbol
# the table does not list gains no entry.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/loadcfg.s -filetype=obj -o %t.loadcfg.obj
# RUN: lld-link %t.main.obj %t.loadcfg.obj -guard:cf -dll -noentry \
# RUN:   -guardsym:cmdline,S -guardsym:unlisted,S -out:%t.dll
# RUN: llvm-readobj --coff-load-config %t.dll | FileCheck %s

# CHECK:      GuardFlags [ (0x10014500)
# CHECK:        CF_FUNCTION_TABLE_SIZE_5BYTES (0x10000000)
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x180001000 flags 1
# CHECK-NEXT:   0x180001010 flags 1
# CHECK-NEXT:   0x180001020{{$}}
# CHECK-NEXT:   0x180001040 flags 3
# CHECK-NEXT: ]

# An argument other than <symbol>,S is ignored with a warning.
# RUN: lld-link %t.main.obj %t.loadcfg.obj -guard:cf -dll -noentry \
# RUN:   -guardsym:cmdline -guardsym:cmdline,X -guardsym:,S -out:%t.warn.dll 2>&1 \
# RUN:   | FileCheck %s --check-prefix=WARN

# WARN: warning: /guardsym: ignoring unsupported argument: cmdline{{$}}
# WARN: warning: /guardsym: ignoring unsupported argument: cmdline,X
# WARN: warning: /guardsym: ignoring unsupported argument: ,S

#--- main.s
        .text
        .globl @feat.00
.set @feat.00, 2048

        .def _getdllprocaddr; .scl 2; .type 32; .endef
        .globl _getdllprocaddr
        .p2align 4
_getdllprocaddr:
        ret

        .def cmdline; .scl 2; .type 32; .endef
        .globl cmdline
        .p2align 4
cmdline:
        ret

        .def taken; .scl 2; .type 32; .endef
        .globl taken
        .p2align 4
taken:
        ret

        .def unlisted; .scl 2; .type 32; .endef
        .globl unlisted
        .p2align 4
unlisted:
        ret

        .def exported; .scl 2; .type 32; .endef
        .globl exported
        .p2align 4
exported:
        ret

        .section .gfids$y,"dr"
        .symidx _getdllprocaddr
        .symidx cmdline
        .symidx taken

        .section .drectve,"dr"
        .ascii " /GUARDSYM:_getdllprocaddr,S"
        .ascii " /GUARDSYM:exported,S"
        .ascii " /EXPORT:exported"

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
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .quad __guard_longjmp_table
        .quad __guard_longjmp_count
        .fill 84, 1, 0
