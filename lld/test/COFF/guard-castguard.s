# REQUIRES: x86

## An image that holds the slot a compiler's report routine for failed cast
## checks reads, which the load configuration names, says that CastGuard is
## present, so that the loader binds the slot to the system's routine; with or
## without Control Flow Guard. An image without the slot does not say so.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/slot.s -filetype=obj -o %t.slot.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/loadcfg.s -filetype=obj -o %t.loadcfg.obj

# RUN: lld-link %t.main.obj %t.slot.obj %t.loadcfg.obj -entry:main -out:%t.exe
# RUN: llvm-readobj --coff-load-config %t.exe | FileCheck %s --check-prefix=SLOT
# RUN: lld-link %t.main.obj %t.slot.obj %t.loadcfg.obj -entry:main -guard:cf \
# RUN:   -out:%t-cf.exe
# RUN: llvm-readobj --coff-load-config %t-cf.exe | FileCheck %s --check-prefix=SLOT-CF
# RUN: lld-link %t.main.obj %t.loadcfg.obj -entry:main -out:%t-none.exe
# RUN: llvm-readobj --coff-load-config %t-none.exe | FileCheck %s --check-prefix=NONE

# SLOT:      GuardFlags [ (0x1000000)
# SLOT-NEXT:   CASTGUARD_PRESENT (0x1000000)
# SLOT-NEXT: ]
# SLOT-CF:      GuardFlags [
# SLOT-CF:        CASTGUARD_PRESENT (0x1000000)
# SLOT-CF:        CF_INSTRUMENTED (0x100)
# NONE:      GuardFlags [ (0x0)
# NONE-NEXT: ]

#--- main.s
        .text
        .globl main
main:
        ret

#--- slot.s
        .section .00cfg,"dr",discard,__castguard_check_failure_os_handled_fptr
        .globl __castguard_check_failure_os_handled_fptr
        .p2align 3
__castguard_check_failure_os_handled_fptr:
        .quad 0

#--- loadcfg.s
        .weak __castguard_check_failure_os_handled_fptr
        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 328
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .quad __guard_longjmp_table
        .quad __guard_longjmp_count
        .fill 112, 1, 0
        .quad __castguard_check_failure_os_handled_fptr
        .fill 16, 1, 0
