# REQUIRES: x86

## Under -guard:cf, in an image whose objects say they have KCFI prefixes, the
## linker overwrites the type words of the KCFI prefix of each function that
## the guard function table does not list with type 0, which no call expects.
## The marker bytes stay.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/nocf.s -filetype=obj -o %t.nocf.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/bad.s -filetype=obj -o %t.bad.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/upstream.s -filetype=obj -o %t.upstream.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj \
# RUN:   --defsym NOREC=1 -o %t.main.norec.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/nocf.s -filetype=obj \
# RUN:   --defsym NOREC=1 -o %t.nocf.norec.obj

# RUN: lld-link %t.main.obj %t.nocf.obj -guard:cf -entry:main \
# RUN:   -export:exported -debug:symtab -opt:noicf -out:%t.exe
# RUN: llvm-objdump -d %t.exe | FileCheck %s --check-prefixes=CHECK,NOICF

## A function called only directly is sealed, one with a second type word too.
# CHECK:      <__cfi_direct>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x0, %eax
# CHECK-EMPTY:
# CHECK-NEXT: <direct>:
# CHECK:      <__cfi_vdirect>:
# CHECK-NEXT:   00 00 addb %al, (%rax)
# CHECK-NEXT:   00 00 addb %al, (%rax)
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x0, %eax
# CHECK-NEXT:   nop
# CHECK-EMPTY:
# CHECK-NEXT: <vdirect>:

## A function whose address is taken in data, in code, by an export, or only
## in a vtable is listed, so it keeps its types.
# CHECK:      <__cfi_indata>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x22222222, %eax
# CHECK:      <__cfi_incode>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x33333333, %eax
# CHECK:      <__cfi_exported>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x44444444, %eax
# CHECK:      <__cfi_virt>:
# CHECK-NEXT:   nopl (%rax)
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x55555555, %eax

## Without identical code folding, a function called only directly is sealed
## even if an identical one is listed, and the image leaves out its prefix.
## With it, the two share one chunk, which stays unsealed because one of them
## is listed.
# NOICF:      <__cfi_icf_listed>:
# NOICF-NEXT:   nopl 0x71c5a06(%rax)
# NOICF-NEXT:   movl $0x77777777, %eax
# NOICF:      <icf_listed>:
# NOICF-NOT:    nopl
# NOICF:      <icf_unlisted>:

## An object without guard metadata lists every function it references.
# CHECK:      <__cfi_nocf_called>:
# CHECK-NEXT:   nopl 0x71c5a06(%rax)
# CHECK-NEXT:   movl $0x88888888, %eax

# RUN: lld-link %t.main.obj %t.nocf.obj -guard:cf -entry:main \
# RUN:   -export:exported -debug:symtab -opt:icf -out:%t.icf.exe
# RUN: llvm-objdump -d %t.icf.exe | FileCheck %s --check-prefix=ICF

# ICF:      <__cfi_icf_{{(un)?}}listed>:
# ICF:        movl $0x77777777, %eax
# ICF-NOT:    movl $0x0, %eax
# ICF:      <main>:

## Without a guard function table, or without an object that says it has
## KCFI prefixes, nothing is sealed.
# RUN: lld-link %t.main.obj %t.nocf.obj -entry:main \
# RUN:   -export:exported -debug:symtab -opt:noicf -out:%t.nocfg.exe
# RUN: llvm-objdump -d %t.nocfg.exe | FileCheck %s --check-prefix=NONE
# RUN: lld-link %t.main.norec.obj %t.nocf.norec.obj -guard:cf -entry:main \
# RUN:   -export:exported -debug:symtab -opt:noicf -out:%t.norec.exe
# RUN: llvm-objdump -d %t.norec.exe | FileCheck %s --check-prefix=NONE

# NONE:      <__cfi_direct>:
# NONE-NEXT:   nopl 0x71c5a06(%rax)
# NONE-NEXT:   movl $0x11111111, %eax
# NONE:      <__cfi_vdirect>:
# NONE-NEXT:   nopl (%rax)
# NONE-NEXT:   nopl 0x71c5a06(%rax)
# NONE-NEXT:   movl $0x66666666, %eax
# NONE:      <__cfi_icf_unlisted>:
# NONE-NEXT:   nopl 0x71c5a06(%rax)
# NONE-NEXT:   movl $0x77777777, %eax

## A prefix without the marker, such as upstream KCFI's, is foreign and left
## as it is.
# RUN: lld-link %t.main.obj %t.nocf.obj %t.upstream.obj -guard:cf \
# RUN:   -entry:main -export:exported -debug:symtab -out:%t.upstream.exe
# RUN: llvm-objdump -d %t.upstream.exe | FileCheck %s --check-prefix=UPSTREAM

# UPSTREAM:      <__cfi_upstream>:
# UPSTREAM-COUNT-11: nop
# UPSTREAM-NEXT:   movl $0x11111111, %eax
# UPSTREAM-EMPTY:
# UPSTREAM-NEXT: <upstream>:

## A prefix with the marker that no function follows is an error.
# RUN: not lld-link %t.main.obj %t.nocf.obj %t.bad.obj -guard:cf \
# RUN:   -entry:main -export:exported -out:%t.bad.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ERR

# ERR: error: {{.*}}.bad.obj: no function follows the KCFI prefix at __cfi_bad

#--- main.s
.ifndef NOREC
  .linktypeprefixes
.endif
        .globl @feat.00
@feat.00 = 0x800

        .def direct; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,direct
        .p2align 4
        .fill 4, 1, 0x90
__cfi_direct:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl direct
direct:
        retq

## With a patchable prefix after the prefix, whose bytes the image then holds.
        .def vdirect; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,vdirect
__cfi_vdirect:
        .long 0x00401f0f
        nopl 0x71c5a06(%rax)
        movl $0x66666666, %eax
        nop
        .globl vdirect
vdirect:
        retq

        .def indata; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,indata
        .p2align 4
        .fill 4, 1, 0x90
__cfi_indata:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl indata
indata:
        retq

        .def incode; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,incode
        .p2align 4
        .fill 4, 1, 0x90
__cfi_incode:
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl incode
incode:
        retq

        .def exported; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,exported
        .p2align 4
        .fill 4, 1, 0x90
__cfi_exported:
        nopl 0x71c5a06(%rax)
        movl $0x44444444, %eax
        .globl exported
exported:
        retq

        .def virt; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,virt
        .p2align 4
__cfi_virt:
        .long 0x00401f0f
        nopl 0x71c5a06(%rax)
        movl $0x55555555, %eax
        .globl virt
virt:
        retq

        .def icf_listed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,icf_listed
        .p2align 4
        .fill 4, 1, 0x90
__cfi_icf_listed:
        nopl 0x71c5a06(%rax)
        movl $0x77777777, %eax
        .globl icf_listed
icf_listed:
        movl $42, %eax
        retq

        .def icf_unlisted; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,icf_unlisted
        .p2align 4
        .fill 4, 1, 0x90
__cfi_icf_unlisted:
        nopl 0x71c5a06(%rax)
        movl $0x77777777, %eax
        .globl icf_unlisted
icf_unlisted:
        movl $42, %eax
        retq

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq direct
        callq vdirect
        callq icf_unlisted
        callq nocf_called
        leaq incode(%rip), %rax
        callq *fp(%rip)
        xorl %eax, %eax
        retq

        .data
fp:
        .quad indata
        .quad icf_listed

        .section .rdata,"dr"
vtable:
        .quad virt

        .section .gfids$y,"dr"
        .symidx indata
        .symidx incode
        .symidx icf_listed
        .symidx virt

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

#--- nocf.s
.ifndef NOREC
  .linktypeprefixes
.endif
        .def nocf_called; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,nocf_called
        .p2align 4
        .fill 4, 1, 0x90
__cfi_nocf_called:
        nopl 0x71c5a06(%rax)
        movl $0x88888888, %eax
        .globl nocf_called
nocf_called:
        retq

        .text
        .globl nocf_user
nocf_user:
        jmp nocf_called

#--- bad.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .text
        .p2align 4
        .fill 4, 1, 0x90
__cfi_bad:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        retq

#--- upstream.s
        .globl @feat.00
@feat.00 = 0x800

        .def upstream; .scl 2; .type 32; .endef
        .text
        .p2align 4
__cfi_upstream:
        .fill 11, 1, 0x90
        movl $0x11111111, %eax
        .globl upstream
upstream:
        retq
