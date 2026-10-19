# REQUIRES: x86
## A function that a member of a joint link imports from a member DLL and takes
## the address of is not suppressed there, whether the member's link line names
## the DLL's import library or the joint link binds the reference: the member
## leaves its entries out of the loader's grants and imports a private export
## of the DLL that names what it does not suppress, so that a DLL linked with
## them suppressed fails to load beside it. A function other members only call
## stays suppressed.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium loadcfg.s -o loadcfg.obj

## Linked apart, b.dll asks the loader to grant the address it loads and the
## one its data holds.
# RUN: lld-link -import-slots -guard:cf -dll -noentry a.obj loadcfg.obj -out:a.dll
# RUN: lld-link -import-slots -guard:cf -dll -noentry b.obj loadcfg.obj a.lib \
# RUN:   -out:b.dll
# RUN: llvm-readobj --coff-load-config b.dll | FileCheck --check-prefix=APART %s
# APART: GuardAddressTakenIatEntryCount: 2

# RUN: rm a.lib
# RUN: lld-link -import-slots -guard:cf -dll -noentry a.obj loadcfg.obj \
# RUN:   -out:a.dll -lldjointscan:a.jscan
# RUN: lld-link -import-slots -guard:cf -dll -noentry b.obj loadcfg.obj a.lib \
# RUN:   -out:b.dll -lldjointscan:b.jscan
# RUN: lld-link -import-slots -guard:cf -dll -noentry c.obj loadcfg.obj \
# RUN:   -out:c.dll -lldjointscan:c.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan c.jscan
# RUN: lld-link -import-slots -guard:cf -dll -noentry a.obj loadcfg.obj \
# RUN:   -out:a.dll -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -guard:cf -dll -noentry b.obj loadcfg.obj a.lib \
# RUN:   -out:b.dll -lldjoint:facts/b.dll.jfacts
# RUN: lld-link -import-slots -guard:cf -dll -noentry c.obj loadcfg.obj \
# RUN:   -out:c.dll -lldjoint:facts/c.dll.jfacts

## a.dll suppresses only ai, which b.dll only calls; neither its import library
## nor its identity names the private export.
# RUN: llvm-readobj --coff-load-config --coff-exports a.dll | \
# RUN:   FileCheck --check-prefix=A %s
# RUN: llvm-nm a.lib | FileCheck --check-prefix=ALIB %s
# A-NOT:  __llvm_private
# A:      Name: __llvm_unsuppressed$[[G:[0-9a-f]+]]
# A-NOT:  __llvm_private
# A:      GuardFidTable [
# A-NEXT:   0x180001000{{$}}
# A-NEXT:   0x180001010{{$}}
# A-NEXT:   0x180001020{{$}}
# A-NEXT:   0x180001030 flags 2
# A-NEXT: ]
# ALIB-NOT: __llvm_unsuppressed

# RUN: llvm-readobj --coff-exports a.dll > b.txt
# RUN: llvm-readobj --coff-load-config --coff-imports b.dll >> b.txt
# RUN: FileCheck --check-prefix=B %s < b.txt
# B:      Name: __llvm_unsuppressed$[[G:[0-9a-f]+]]
# B-DAG:  GuardAddressTakenIatEntryCount: 0
# B-DAG:  Symbol: __llvm_unsuppressed$[[G]] (0)

# RUN: llvm-readobj --coff-exports a.dll > c.txt
# RUN: llvm-readobj --coff-load-config --coff-imports c.dll >> c.txt
# RUN: FileCheck --check-prefix=B %s < c.txt

#--- a.s
        .globl @feat.00
.set @feat.00, 2048
        .text
        .def af; .scl 2; .type 32; .endef
        .globl af
        .p2align 4
af:
        retq
        .def ag; .scl 2; .type 32; .endef
        .globl ag
        .p2align 4
ag:
        retq
        .def ah; .scl 2; .type 32; .endef
        .globl ah
        .p2align 4
ah:
        retq
        .def ai; .scl 2; .type 32; .endef
        .globl ai
        .p2align 4
ai:
        retq

        .section .drectve,"yni"
        .ascii " -export:af -export:ag -export:ah -export:ai"

#--- b.s
        .globl @feat.00
.set @feat.00, 2048
        .text
        .def bf; .scl 2; .type 32; .endef
        .globl bf
        .p2align 4
bf:
        movq __imp_af(%rip), %rax
        callq *__imp_ag(%rip)
        callq *__imp_ai(%rip)
        retq

        .data
        .quad ah

        .section .giats$y,"dr"
        .symidx __imp_af

        .section .drectve,"yni"
        .ascii " -export:bf"

#--- c.s
        .globl @feat.00
.set @feat.00, 2048
        .text
        .def cf; .scl 2; .type 32; .endef
        .globl cf
        .p2align 4
cf:
        movq __imp_ag(%rip), %rax
        retq

        .section .giats$y,"dr"
        .symidx __imp_ag

        .section .drectve,"yni"
        .ascii " -export:cf"

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
