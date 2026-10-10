# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj

# /guard:ehcont,nocf asks for the EH continuation table without Control Flow
# Guard, which a shadow stack checks continuations against whether or not
# calls are checked.
# RUN: lld-link %t.obj -guard:ehcont,nocf -out:%t.exe -entry:main
# RUN: llvm-readobj --file-headers --coff-load-config %t.exe \
# RUN:   | FileCheck %s --check-prefix=EHCONT

# EHCONT-NOT:  IMAGE_DLL_CHARACTERISTICS_GUARD_CF
# EHCONT:      GuardCFFunctionTable: 0x0
# EHCONT:      GuardCFFunctionCount: 0
# EHCONT:      GuardFlags [ (0x400000)
# EHCONT-NEXT:   EH_CONTINUATION_TABLE_PRESENT (0x400000)
# EHCONT-NEXT: ]
# EHCONT:      GuardEHContinuationTable: 0x14000{{.*}}
# EHCONT-NEXT: GuardEHContinuationCount: 2

# RUN: lld-link %t.obj -guard:cf,ehcont -out:%t-cf.exe -entry:main
# RUN: llvm-readobj --coff-load-config %t-cf.exe \
# RUN:   | FileCheck %s --check-prefix=CF

# CF:      GuardFlags [ (0x414500)
# CF-NEXT:   CF_EXPORT_SUPPRESSION_INFO_PRESENT (0x4000)
# CF-NEXT:   CF_FUNCTION_TABLE_PRESENT (0x400)
# CF-NEXT:   CF_INSTRUMENTED (0x100)
# CF-NEXT:   CF_LONGJUMP_TABLE_PRESENT (0x10000)
# CF-NEXT:   EH_CONTINUATION_TABLE_PRESENT (0x400000)
# CF-NEXT: ]
# CF:      GuardEHContinuationCount: 2

# -cetcompat alone asks for no table.
# RUN: lld-link %t.obj -cetcompat -out:%t-cet.exe -entry:main
# RUN: llvm-readobj --coff-load-config %t-cet.exe \
# RUN:   | FileCheck %s --check-prefix=NONE

# NONE:      GuardFlags [ (0x0)
# NONE-NEXT: ]
# NONE:      GuardEHContinuationTable: 0x0
# NONE-NEXT: GuardEHContinuationCount: 0

# We need @feat.00 to have 0x4000 to indicate /guard:ehcont.
        .def     @feat.00;
        .scl    3;
        .type   0;
        .endef
        .globl  @feat.00
@feat.00 = 0x4000
        .def     main; .scl    2; .type   32; .endef
        .globl	main                            # -- Begin function main
        .p2align	4, 0x90
main:
.seh_proc main
        .seh_handler __C_specific_handler, @unwind, @except
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
        .text
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
