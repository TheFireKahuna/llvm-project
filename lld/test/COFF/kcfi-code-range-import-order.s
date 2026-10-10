# REQUIRES: x86

## A KCFI thunk tests the code ranges of at most four DLLs, those with the most
## functions of its type, in load order among equals, and only DLLs that a
## thunk tests are bound. d0 to d4 have 1, 3, 2, 3 and 1 functions of type
## 0x11111111, so its thunk tests d1, d3, d2 and d0, and d4 is not bound. d2
## alone has a function of second type 0x66666666, which the vfn thunk tests.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc d0.s -filetype=obj -o d0.obj
# RUN: lld-link d0.obj -dll -noentry -export:d0f0 -guard:cf \
# RUN:   -out:d0.dll -implib:d0.lib
# RUN: llvm-mc -triple x86_64-windows-msvc d1.s -filetype=obj -o d1.obj
# RUN: lld-link d1.obj -dll -noentry -export:d1f0 -export:d1f1 -export:d1f2 -guard:cf \
# RUN:   -out:d1.dll -implib:d1.lib
# RUN: llvm-mc -triple x86_64-windows-msvc d2.s -filetype=obj -o d2.obj
# RUN: lld-link d2.obj -dll -noentry -export:d2f0 -export:d2f1 -export:d2v -guard:cf \
# RUN:   -out:d2.dll -implib:d2.lib
# RUN: llvm-mc -triple x86_64-windows-msvc d3.s -filetype=obj -o d3.obj
# RUN: lld-link d3.obj -dll -noentry -export:d3f0 -export:d3f1 -export:d3f2 -guard:cf \
# RUN:   -out:d3.dll -implib:d3.lib
# RUN: llvm-mc -triple x86_64-windows-msvc d4.s -filetype=obj -o d4.obj
# RUN: lld-link d4.obj -dll -noentry -export:d4f0 -guard:cf \
# RUN:   -out:d4.dll -implib:d4.lib
# RUN: llvm-mc -triple x86_64-windows-msvc exe.s -filetype=obj -o exe.obj
# RUN: lld-link exe.obj d0.lib d1.lib d2.lib d3.lib d4.lib -guard:cf \
# RUN:   -entry:main -debug:symtab -out:exe.exe
# RUN: llvm-readobj --coff-imports exe.exe > exe.txt
# RUN: llvm-objdump -d exe.exe >> exe.txt
# RUN: FileCheck %s < exe.txt

# CHECK:      Name: d0.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,D0:]]
# CHECK-NEXT: Symbol: __llvm_code_start
# CHECK:      Name: d1.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,D1:]]
# CHECK-NEXT: Symbol: __llvm_code_start
# CHECK:      Name: d2.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,D2:]]
# CHECK-NEXT: Symbol: __llvm_code_start
# CHECK:      Name: d3.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,D3:]]
# CHECK-NEXT: Symbol: __llvm_code_start
# CHECK:      Name: d4.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: d4f0
# CHECK-NEXT: }

# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+D1]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+D3]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+D2]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+D0]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq
# CHECK-NEXT:   jb
# CHECK-NEXT:   testl $0xff0, %eax

# CHECK:      <__llvm_kcfi_vfn_check_66666666>:
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,0x140000000+D2]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,0x140000008+D2]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   testl $0xff0, %ecx
# CHECK:        jmpq *{{.*}}(%rip) {{.*}}<__guard_check_icall_fptr>
# CHECK-NEXT:   movabsq $0x6801f0f66666666, %r11
# CHECK-NEXT:   cmpq %r11, -0x10(%rcx)
# CHECK-NEXT:   jne
# CHECK-NEXT:   retq

#--- d0.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def d0f0; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d0f0
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d0f0:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d0f0
d0f0:
        movl $1, %ecx
        retq

        .section .gfids$y,"dr"
        .symidx d0f0

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

#--- d1.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def d1f0; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d1f0
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d1f0:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d1f0
d1f0:
        movl $2, %ecx
        retq

        .def d1f1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d1f1
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d1f1:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d1f1
d1f1:
        movl $3, %ecx
        retq

        .def d1f2; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d1f2
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d1f2:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d1f2
d1f2:
        movl $4, %ecx
        retq

        .section .gfids$y,"dr"
        .symidx d1f0
        .symidx d1f1
        .symidx d1f2

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

#--- d2.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def d2f0; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d2f0
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d2f0:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d2f0
d2f0:
        movl $5, %ecx
        retq

        .def d2f1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d2f1
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d2f1:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d2f1
d2f1:
        movl $6, %ecx
        retq

        .def d2v; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d2v
        .p2align 4
__cfi_d2v:
        .long 0x66666666
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl d2v
d2v:
        movl $7, %ecx
        retq

        .section .gfids$y,"dr"
        .symidx d2f0
        .symidx d2f1
        .symidx d2v

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

#--- d3.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def d3f0; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d3f0
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d3f0:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d3f0
d3f0:
        movl $8, %ecx
        retq

        .def d3f1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d3f1
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d3f1:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d3f1
d3f1:
        movl $9, %ecx
        retq

        .def d3f2; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d3f2
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d3f2:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d3f2
d3f2:
        movl $10, %ecx
        retq

        .section .gfids$y,"dr"
        .symidx d3f0
        .symidx d3f1
        .symidx d3f2

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

#--- d4.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def d4f0; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d4f0
        .p2align 4
        .fill 4, 1, 0x90
__cfi_d4f0:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl d4f0
d4f0:
        movl $11, %ecx
        retq

        .section .gfids$y,"dr"
        .symidx d4f0

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
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq *__imp_d0f0(%rip)
        callq *__imp_d1f0(%rip)
        callq *__imp_d2f0(%rip)
        callq *__imp_d3f0(%rip)
        callq *__imp_d4f0(%rip)
        callq __llvm_kcfi_dispatch_11111111
        callq __llvm_kcfi_vfn_check_66666666
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_open
        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        .linkkcfithunk __llvm_kcfi_dispatch_11111111, dispatch, 0x11111111, 0x071c5a06, 0, __llvm_kcfi_mismatch_11111111
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_11111111
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .weak __llvm_kcfi_check_mismatch_66666666
__llvm_kcfi_check_mismatch_66666666 = __llvm_kcfi_open
        .section .text,"xr",discard,__llvm_kcfi_vfn_check_66666666
        .globl __llvm_kcfi_vfn_check_66666666
        .p2align 4
__llvm_kcfi_vfn_check_66666666:
        .linkkcfithunk __llvm_kcfi_vfn_check_66666666, vfn_check, 0x66666666, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_66666666
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rcx
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rcx
        jae 1f
        movabsq $0x06801f0f66666666, %r11
        cmpq %r11, -16(%rcx)
        jne __llvm_kcfi_check_mismatch_66666666
        retq
1:      testl $0xff0, %ecx
        je __llvm_kcfi_check_mismatch_66666666
        movabsq $0x06801f0f66666666, %r11
        cmpq %r11, -16(%rcx)
        jne __llvm_kcfi_check_mismatch_66666666
        jmpq *__guard_check_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        int3

        .section .rdata,"dr"
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad 0

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
