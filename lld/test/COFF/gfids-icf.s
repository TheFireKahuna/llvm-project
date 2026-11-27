# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf,nolongjmp -out:%t.exe -opt:icf -entry:main
# RUN: llvm-readobj --file-headers --coff-load-config %t.exe | FileCheck %s --check-prefix=CHECK

# This assembly is meant to mimic what CL emits for this kind of C code:
# int icf1() { return 42; }
# int icf2() { return 42; }
# int (*fp1)() = &icf1;
# int (*fp2)() = &icf2;
# int main() {
#   return fp1();
#   return fp2();
# }

# 'icf1' and 'icf2' are address taken, but should be merged into one entry.
# There are two entries in the table because 'main' is included.

# CHECK: ImageBase: 0x140000000
# CHECK: LoadConfig [
# CHECK:   SEHandlerTable: 0x0
# CHECK:   SEHandlerCount: 0
# CHECK:   GuardCFCheckFunction: 0x0
# CHECK:   GuardCFCheckDispatch: 0x0
# CHECK:   GuardCFFunctionTable: 0x14000{{.*}}
# CHECK:   GuardCFFunctionCount: 2
# CHECK:   GuardFlags [ (0x4500)
# CHECK:     CF_FUNCTION_TABLE_PRESENT (0x400)
# CHECK:     CF_INSTRUMENTED (0x100)
# CHECK:   ]
# CHECK:   GuardAddressTakenIatEntryTable: 0x0
# CHECK:   GuardAddressTakenIatEntryCount: 0
# CHECK:   GuardLongJumpTargetTable: 0x0
# CHECK:   GuardLongJumpTargetCount: 0
# CHECK: ]
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x14000{{.*}}
# CHECK-NEXT:   0x14000{{.*}}
# CHECK-NEXT: ]

# In an object for Windows Itanium, whose address-significance table alone
# says which addresses are significant, they fold too; exported, they do not.
# RUN: llvm-mc -triple x86_64-windows-itanium %s -filetype=obj -o %t.fork.obj
# RUN: lld-link %t.fork.obj -guard:cf,nolongjmp -out:%t.fork.exe -opt:icf -entry:main
# RUN: llvm-readobj --file-headers --coff-load-config %t.fork.exe | FileCheck %s --check-prefix=CHECK
# RUN: lld-link %t.fork.obj -guard:cf,nolongjmp -out:%t.fork-exp.exe -opt:icf -entry:main -export:icf1 -export:icf2
# RUN: llvm-readobj --coff-exports %t.fork-exp.exe | FileCheck %s --check-prefix=EXPORTS
# RUN: llvm-readobj --coff-load-config %t.fork-exp.exe | FileCheck %s --check-prefix=EXPORTS-FIDS

# EXPORTS:      Name: icf1
# EXPORTS-NEXT: RVA: 0x[[#%X,ICF1:]]
# EXPORTS:      Name: icf2
# EXPORTS-NEXT: RVA: 0x[[#%X,ICF1+16]]
# EXPORTS-FIDS: GuardCFFunctionCount: 3

# A function /guardsym suppresses does not fold with a valid one.
# RUN: lld-link %t.obj -guard:cf,nolongjmp -out:%t-sym.exe -opt:icf -entry:main -guardsym:icf2,S
# RUN: llvm-readobj --coff-load-config %t-sym.exe | FileCheck %s --check-prefix=SYM

# SYM:      GuardFidTable [
# SYM-NEXT:   0x14000{{.*}}
# SYM-NEXT:   0x14000{{.*}} flags 1
# SYM-NEXT:   0x14000{{.*}}
# SYM-NEXT: ]

# Two suppressed functions fold.
# RUN: lld-link %t.obj -guard:cf,nolongjmp -out:%t-sym2.exe -opt:icf -entry:main -guardsym:icf1,S -guardsym:icf2,S
# RUN: llvm-readobj --coff-load-config %t-sym2.exe | FileCheck %s --check-prefix=SYM2

# SYM2:      GuardFidTable [
# SYM2-NEXT:   0x14000{{.*}} flags 1
# SYM2-NEXT:   0x14000{{.*}}{{$}}
# SYM2-NEXT: ]

# There should be no .gfids section in the output executable when we link with
# -guard:cf or with no -guard:cf/nolongjmp flag.
# RUN: llvm-readobj --sections %t.exe | FileCheck %s --check-prefix NOGFIDSEC
# RUN: lld-link %t.obj -out:%t.exe -opt:icf -entry:main
# RUN: llvm-readobj --sections %t.exe | FileCheck %s --check-prefix NOGFIDSEC

# NOGFIDSEC: Sections [
# NOGFIDSEC: Section {
# NOGFIDSEC: Name: .text
# NOGFIDSEC-NOT: Name: .gfids

# Indicate that gfids are present.
        .def     @feat.00; .scl    3; .type   0; .endef
        .globl  @feat.00
@feat.00 = 0x800

        .def     icf1; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,icf1
        .global icf1
icf1:
        movl $42, %eax
        retq

        .def     icf2; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,icf2
        .global icf2
icf2:
        movl $42, %eax
        retq

# Take their two addresses.
        .data
        .globl  fp1
fp1:
        .quad   icf1
        .globl  fp2
fp2:
        .quad   icf2

        .section        .gfids$y,"dr"
        .symidx icf1
        .symidx icf2

        .def     main; .scl    2; .type   32; .endef
        .section        .text,"xr",one_only,main
        .globl  main
main:
        callq      *fp1(%rip)
        callq      *fp2(%rip)
        xor %eax, %eax
        retq

        .section .rdata,"dr"
.globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0

        .addrsig
