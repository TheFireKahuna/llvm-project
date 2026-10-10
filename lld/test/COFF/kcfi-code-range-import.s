# REQUIRES: x86

## With a guard function table, an image's KCFI thunk
## tests, after its own code range, the ranges of the DLLs it imports
## statically whose import libraries record an unsealed function of the
## thunk's type there, those with the most such functions first. The image
## imports the bounds of each such range as data, start then end, in one
## 16-byte block of the DLL's existing import address table. A target inside
## any of the ranges takes the type check and the jump; one outside them all
## takes the page test, the type check and the guard function.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc a.s -filetype=obj -o a.obj
# RUN: llvm-mc -triple x86_64-windows-msvc b.s -filetype=obj -o b.obj
# RUN: llvm-mc -triple x86_64-windows-msvc exe.s -filetype=obj -o exe.obj
# RUN: llvm-mc -triple x86_64-windows-msvc exe.s -filetype=obj --defsym NOREC=1 \
# RUN:   -o exe.norec.obj
# RUN: lld-link a.obj -dll -noentry -export:a1 -guard:cf \
# RUN:   -out:a.dll -implib:a.lib
# RUN: lld-link b.obj -dll -noentry -export:b1 -export:b2 -guard:cf \
# RUN:   -out:b.dll -implib:b.lib

# RUN: lld-link exe.obj a.lib b.lib -guard:cf -entry:main \
# RUN:   -debug:symtab -out:exe.exe
# RUN: llvm-readobj --coff-imports exe.exe > exe.txt
# RUN: FileCheck %s --check-prefix=ALIGN < exe.txt
# RUN: llvm-objdump -d exe.exe >> exe.txt
# RUN: FileCheck %s < exe.txt

## Each DLL's descriptor holds the bounds, 16-byte aligned, by their hints:
## a.dll's four entries, with the terminator, keep b.dll's pair aligned first.
# ALIGN:      Name: a.dll
# ALIGN:      ImportAddressTableRVA: 0x{{[0-9A-F]*}}0{{$}}
# ALIGN:      Name: b.dll
# ALIGN:      ImportAddressTableRVA: 0x{{[0-9A-F]*}}0{{$}}
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,A_IAT:]]
# CHECK-NEXT: Symbol: __llvm_code_start (1)
# CHECK-NEXT: Symbol: __llvm_code_end (0)
# CHECK-NEXT: Symbol: a1 (2)
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,B_IAT:]]
# CHECK-NEXT: Symbol: __llvm_code_start (1)
# CHECK-NEXT: Symbol: __llvm_code_end (0)
# CHECK-NEXT: Symbol: b1 (2)
# CHECK-NOT:  Name:

## Type 0x11111111: the image's range, then a.dll's, with two functions of
## the type, then b.dll's, with one.
# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   movq %rax, %r11
# CHECK-NEXT:   subq %r10, %r11
# CHECK-NEXT:   cmpq $0x{{[0-9a-f]+}}, %r11
# CHECK-NEXT:   jae 0x[[IMPORTED:[0-9a-f]+]]
# CHECK-NEXT:   [[HIT:[0-9a-f]+]]: {{.*}}movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne {{.*}}<__llvm_kcfi_open>
# CHECK-NEXT:   jmpq *%rax
# CHECK-NEXT:   [[IMPORTED]]: {{.*}}cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+A_IAT]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000008+A_IAT]]
# CHECK-NEXT:   jb 0x[[HIT]]
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+B_IAT]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000008+B_IAT]]
# CHECK-NEXT:   jb 0x[[HIT]]
# CHECK-NEXT:   testl $0xff0, %eax
# CHECK-NEXT:   je {{.*}}<__llvm_kcfi_open>
# CHECK-NEXT:   movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne {{.*}}<__llvm_kcfi_open>
# CHECK-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>

## Type 0x22222222 has no function in any range.
# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK-NEXT:   testl $0xff0, %eax

## Type 0x44444444 has one only in b.dll's, so the image's own range is not
## tested, and the type check and jump follow the guard function.
# CHECK:      <__llvm_kcfi_dispatch_44444444>:
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000000+B_IAT]]
# CHECK-NEXT:   jb
# CHECK-NEXT:   cmpq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,0x140000008+B_IAT]]
# CHECK-NEXT:   jb 0x[[HIT44:[0-9a-f]+]]
# CHECK-NEXT:   testl $0xff0, %eax
# CHECK-NEXT:   je {{.*}}<__llvm_kcfi_open>
# CHECK-NEXT:   movabsq $0x44444444b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne {{.*}}<__llvm_kcfi_open>
# CHECK-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>
# CHECK-NEXT:   [[HIT44]]: {{.*}}movabsq $0x44444444b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne {{.*}}<__llvm_kcfi_open>
# CHECK-NEXT:   jmpq *%rax

## A delay-loaded DLL's range, and every range of an image without a guard
## function table or without an object that says it has KCFI prefixes, are not
## bound.
# RUN: llvm-mc -triple x86_64-windows-msvc helper.s -filetype=obj -o helper.obj
# RUN: lld-link exe.obj helper.obj a.lib b.lib -guard:cf \
# RUN:   -entry:main -delayload:b.dll -out:delay.exe
# RUN: llvm-readobj --coff-imports delay.exe | FileCheck %s --check-prefix=DELAY
# DELAY:      Name: a.dll
# DELAY:      Symbol: __llvm_code_start
# DELAY:      DelayImport {
# DELAY-NEXT:   Name: b.dll
# DELAY-NOT:  __llvm_code
# RUN: lld-link exe.obj a.lib b.lib -entry:main -out:noguard.exe
# RUN: llvm-readobj --coff-imports noguard.exe | FileCheck %s --check-prefix=NONE
# RUN: lld-link exe.norec.obj a.lib b.lib -guard:cf -entry:main -out:norec.exe
# RUN: llvm-readobj --coff-imports norec.exe | FileCheck %s --check-prefix=NONE
# NONE-NOT: __llvm_code

## A DLL whose import library has no record, such as one written from a
## module-definition file, is not bound.
# RUN: lld-link -def:b.def -machine:x64 -out:b.def.lib
# RUN: lld-link exe.obj a.lib b.def.lib -guard:cf -entry:main \
# RUN:   -out:norecord.exe
# RUN: llvm-readobj --coff-imports norecord.exe | FileCheck %s --check-prefix=NORECORD
# NORECORD:      Name: a.dll
# NORECORD:      Symbol: __llvm_code_start
# NORECORD:      Name: b.dll
# NORECORD-NOT:  __llvm_code

#--- b.def
LIBRARY b.dll
EXPORTS
        b1

#--- helper.s
        .text
        .globl __delayLoadHelper2
__delayLoadHelper2:
        retq

#--- a.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def a1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,a1
        .p2align 4
        .fill 4, 1, 0x90
__cfi_a1:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl a1
a1:
        retq

        .def a2; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,a2
        .p2align 4
        .fill 4, 1, 0x90
__cfi_a2:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl a2
a2:
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .section .gfids$y,"dr"
        .symidx a1
        .symidx a2

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

#--- b.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def b1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,b1
        .p2align 4
        .fill 4, 1, 0x90
__cfi_b1:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl b1
b1:
        retq

        .def b2; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,b2
        .p2align 4
        .fill 4, 1, 0x90
__cfi_b2:
        nopl 0x71c5a06(%rax)
        movl $0x44444444, %eax
        .globl b2
b2:
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .section .gfids$y,"dr"
        .symidx b1
        .symidx b2

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
.ifndef NOREC
  .linktypeprefixes
.endif
        .globl @feat.00
@feat.00 = 0x800

        .def own; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,own
        .p2align 4
        .fill 4, 1, 0x90
__cfi_own:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl own
own:
        retq

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq *__imp_a1(%rip)
        callq *__imp_b1(%rip)
        movq fp(%rip), %rax
        callq __llvm_kcfi_dispatch_11111111
        callq __llvm_kcfi_dispatch_22222222
        callq __llvm_kcfi_dispatch_44444444
        xorl %eax, %eax
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

        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_open
        .section .text,"xr",discard,__llvm_kcfi_dispatch_22222222
        .globl __llvm_kcfi_dispatch_22222222
        .p2align 4
__llvm_kcfi_dispatch_22222222:
        .linkkcfithunk __llvm_kcfi_dispatch_22222222, dispatch, 0x22222222, 0x071c5a06, 0, __llvm_kcfi_mismatch_22222222
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x22222222b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_22222222
        movabsq $0x22222222b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .weak __llvm_kcfi_mismatch_44444444
__llvm_kcfi_mismatch_44444444 = __llvm_kcfi_open
        .section .text,"xr",discard,__llvm_kcfi_dispatch_44444444
        .globl __llvm_kcfi_dispatch_44444444
        .p2align 4
__llvm_kcfi_dispatch_44444444:
        .linkkcfithunk __llvm_kcfi_dispatch_44444444, dispatch, 0x44444444, 0x071c5a06, 0, __llvm_kcfi_mismatch_44444444
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x44444444b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_44444444
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_44444444
        movabsq $0x44444444b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_44444444
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        int3

        .data
fp:
        .quad own

        .section .rdata,"dr"
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0

        .section .gfids$y,"dr"
        .symidx own

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
