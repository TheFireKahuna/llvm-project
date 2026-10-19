# REQUIRES: aarch64
## On ARM64, as on x86-64, a member of a joint link that takes the address of a
## function another member exports, by a load of its import or a word of its
## data, leaves those entries out of the loader's grants, and the exporter does
## not suppress the function.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium loadcfg.s \
# RUN:   -o loadcfg.obj
# RUN: lld-link -machine:arm64 -import-slots -guard:cf -dll -noentry a.obj \
# RUN:   loadcfg.obj -out:a.dll -lldjointscan:a.jscan
# RUN: lld-link -machine:arm64 -import-slots -guard:cf -dll -noentry b.obj \
# RUN:   loadcfg.obj a.lib -out:b.dll -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -machine:arm64 -import-slots -guard:cf -dll -noentry a.obj \
# RUN:   loadcfg.obj -out:a.dll -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -machine:arm64 -import-slots -guard:cf -dll -noentry b.obj \
# RUN:   loadcfg.obj a.lib -out:b.dll -lldjoint:facts/b.dll.jfacts
# RUN: llvm-readobj --coff-load-config a.dll | FileCheck --check-prefix=A %s
# RUN: llvm-readobj --coff-load-config b.dll | FileCheck --check-prefix=B %s

# A:      GuardFidTable [
# A-NEXT:   0x180001000{{$}}
# A-NEXT:   0x180001010 flags 2
# A-NEXT:   0x180001020{{$}}
# A-NEXT: ]
# B: GuardAddressTakenIatEntryCount: 0

#--- a.s
        .globl @feat.00
.set @feat.00, 2048
        .text
        .def af; .scl 2; .type 32; .endef
        .globl af
        .p2align 4
af:
        ret
        .def ag; .scl 2; .type 32; .endef
        .globl ag
        .p2align 4
ag:
        ret
        .def ah; .scl 2; .type 32; .endef
        .globl ah
        .p2align 4
ah:
        ret

        .section .drectve,"yni"
        .ascii " -export:af -export:ag -export:ah"

#--- b.s
        .globl @feat.00
.set @feat.00, 2048
        .text
        .def bf; .scl 2; .type 32; .endef
        .globl bf
        .p2align 4
bf:
        adrp x0, __imp_af
        ldr x0, [x0, :lo12:__imp_af]
        adrp x16, __imp_ag
        ldr x16, [x16, :lo12:__imp_ag]
        br x16

        .data
        .xword ah

        .section .giats$y,"dr"
        .symidx __imp_af

        .section .drectve,"yni"
        .ascii " -export:bf"

#--- loadcfg.s
        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .word 256
        .fill 124, 1, 0
        .xword __guard_fids_table
        .xword __guard_fids_count
        .word __guard_flags
        .fill 12, 1, 0
        .xword __guard_iat_table
        .xword __guard_iat_count
        .xword __guard_longjmp_table
        .xword __guard_longjmp_count
        .fill 84, 1, 0
