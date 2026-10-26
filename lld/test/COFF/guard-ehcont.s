# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -guard:ehcont -out:%t.exe -entry:main
# RUN: llvm-readobj --file-headers --coff-load-config %t.exe | FileCheck %s

# /guard:nocf keeps the table and drops CFG.
# RUN: lld-link %t.obj -guard:ehcont,nocf -out:%t-nocf.exe -entry:main
# RUN: llvm-readobj --file-headers --coff-load-config %t-nocf.exe | FileCheck %s --check-prefix=NOCF

# NOCF-NOT:  IMAGE_DLL_CHARACTERISTICS_GUARD_CF
# NOCF:      GuardCFFunctionTable: 0x0
# NOCF:      GuardFlags [ (0x400000)
# NOCF:      GuardEHContinuationCount: 2

# Without the metadata, SEH in a COMDAT takes the __except targets of its scope
# table, with a warning; outside a COMDAT, and with _local_unwind, it is an
# error.
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t-comdat.obj --defsym NOEHCONT=1 --defsym COMDAT=1
# RUN: lld-link %t-comdat.obj -guard:cf -guard:ehcont -out:%t-comdat.exe -entry:main 2>&1 | FileCheck %s --check-prefix=WARN
# RUN: llvm-readobj --file-headers --coff-load-config %t-comdat.exe | FileCheck %s
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t-text.obj --defsym NOEHCONT=1
# RUN: not lld-link %t-text.obj -guard:cf -guard:ehcont -out:%t-text.exe -entry:main 2>&1 | FileCheck %s --check-prefix=ERR
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t-unwind.obj --defsym NOEHCONT=1 --defsym COMDAT=1 --defsym LOCAL_UNWIND=1
# RUN: not lld-link %t-unwind.obj -guard:cf -guard:ehcont -out:%t-unwind.exe -entry:main 2>&1 | FileCheck %s --check-prefix=UNWIND
# __GSHandlerCheck has no continuation targets.
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t-gs.obj --defsym NOEHCONT=1 --defsym GS=1
# RUN: lld-link %t-gs.obj -guard:cf -guard:ehcont -out:%t-gs.exe -entry:main 2>&1 | FileCheck %s --check-prefix=GS --allow-empty

# WARN: warning: {{.*}}comdat.obj: object has no EH continuation metadata; adding the __except targets of its SEH in COMDAT sections
# ERR: error: {{.*}}text.obj: object has no EH continuation metadata but needs it for __C_specific_handler under /guard:ehcont
# UNWIND: error: {{.*}}unwind.obj: object has no EH continuation metadata but needs it for _local_unwind under /guard:ehcont
# GS-NOT: EH continuation

# CHECK: ImageBase: 0x140000000
# CHECK: LoadConfig [
# CHECK:   SEHandlerTable: 0x0
# CHECK:   SEHandlerCount: 0
# CHECK:   GuardCFCheckFunction: 0x0
# CHECK:   GuardCFCheckDispatch: 0x0
# CHECK:   GuardCFFunctionTable: 0x14000{{.*}}
# CHECK:   GuardCFFunctionCount: 1
# CHECK:   GuardFlags [ (0x404500)
# CHECK:     CF_FUNCTION_TABLE_PRESENT (0x400)
# CHECK:     CF_INSTRUMENTED (0x100)
# CHECK:     EH_CONTINUATION_TABLE_PRESENT (0x400000)
# CHECK:   ]
# CHECK:   GuardAddressTakenIatEntryTable: 0x0
# CHECK:   GuardAddressTakenIatEntryCount: 0
# CHECK:   GuardEHContinuationTable: 0x14000{{.*}}
# CHECK:   GuardEHContinuationCount: 2
# CHECK: ]
# CHECK:      GuardEHContTable [
# CHECK-NEXT:   0x14000{{.*}}
# CHECK-NEXT:   0x14000{{.*}}
# CHECK-NEXT: ]

# We need @feat.00 to have 0x4000 to indicate /guard:ehcont.
        .def     @feat.00;
        .scl    3;
        .type   0;
        .endef
        .globl  @feat.00
.ifdef NOEHCONT
@feat.00 = 0
.else
@feat.00 = 0x4000
.endif
.ifdef LOCAL_UNWIND
        .globl _local_unwind
_local_unwind:
        call _local_unwind
.endif
        .def     main; .scl    2; .type   32; .endef
        .globl	main                            # -- Begin function main
.ifdef COMDAT
        .section .text,"xr",one_only,main
.endif
        .p2align	4, 0x90
main:
.seh_proc main
.ifdef GS
        .seh_handler __GSHandlerCheck, @unwind, @except
.else
        .seh_handler __C_specific_handler, @unwind, @except
.endif
        .seh_handlerdata
        .long 2
        .long (seh_begin)@IMGREL
        .long (seh_end)@IMGREL
        .long 1
        .long (seh_except)@IMGREL
        .long (seh2_begin)@IMGREL
        .long (seh2_end)@IMGREL
        .long 1
        .long (seh2_except)@IMGREL
.ifdef COMDAT
        .section .text,"xr",one_only,main
.else
        .text
.endif
    seh_begin:
        nop
        int3
        nop
    seh_end:
        nop
    seh_except:
        nop

    seh2_begin:
        nop
        int3
        nop
    seh2_end:
        nop
    seh2_except:
        nop

        xor %eax, %eax
        ret
.seh_endproc

__C_specific_handler:
__GSHandlerCheck:
        ret

.section	.gehcont$y,"dr"
.symidx	seh_except
.symidx	seh2_except

.section  .rdata,"dr"
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
