# REQUIRES: x86

## Control Flow Guard tables in an image linked from objects that describe
## their instruction sites. Such an object's sites say which instructions take
## an address: in an object with guard metadata they add the addresses the
## compiler did not list, and in one without, a branch or a call through a
## pointer takes none. A local import pointer in .giats$y is no import address
## table entry; the address of the symbol it holds is taken.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium asm.s -o asm.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc other.s -o other.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc defs.s -o defs.obj
# RUN: lld-link -guard:cf -entry:main -subsystem:console -out:a.exe \
# RUN:   -map:a.map main.obj asm.obj other.obj defs.obj -include:asmfn \
# RUN:   -include:otherfn
# RUN: llvm-readobj --coff-load-config a.exe > lc.txt
# RUN: cat a.map lc.txt | FileCheck %s

# CHECK-DAG: main [[#%x,MAIN:]]
# CHECK-DAG: f [[#%x,F:]]
# CHECK-DAG: g [[#%x,G:]]
# CHECK-DAG: h3 [[#%x,H3:]]
# CHECK-DAG: k [[#%x,K:]]

## main is the entry point; f's address is taken by the load through its
## local import pointer, now a lea, g's and k's by a lea, and h3's, perhaps, by
## an object that does not describe its sites. h, h2 and m are only branched
## to or called.
# CHECK:      GuardAddressTakenIatEntryCount: 0
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x[[#MAIN]]
# CHECK-NEXT:   0x[[#F]]
# CHECK-NEXT:   0x[[#G]]
# CHECK-NEXT:   0x[[#H3]]
# CHECK-NEXT:   0x[[#K]]
# CHECK-NEXT: ]

#--- main.s
  .def @feat.00; .scl 3; .type 0; .endef
  .globl @feat.00
  .set @feat.00, 0x800
  .text
  .def main; .scl 2; .type 32; .endef
  .globl main
main:
  movq __imp_f(%rip), %rax
  leaq g(%rip), %rcx
  callq h
  retq
  .section .giats$y,"dr"
  .symidx __imp_f

#--- asm.s
  .text
  .globl asmfn
asmfn:
  callq h2
  leaq k(%rip), %rax
  callq *__imp_m(%rip)
  retq

#--- other.s
  .text
  .globl otherfn
otherfn:
  callq h3
  retq

#--- defs.s
  .def @feat.00; .scl 3; .type 0; .endef
  .globl @feat.00
  .set @feat.00, 0x800
  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
  .p2align 4
f:
  retq
  .def g; .scl 2; .type 32; .endef
  .globl g
  .p2align 4
g:
  retq
  .def h; .scl 2; .type 32; .endef
  .globl h
  .p2align 4
h:
  retq
  .def h2; .scl 2; .type 32; .endef
  .globl h2
  .p2align 4
h2:
  retq
  .def h3; .scl 2; .type 32; .endef
  .globl h3
  .p2align 4
h3:
  retq
  .def k; .scl 2; .type 32; .endef
  .globl k
  .p2align 4
k:
  retq
  .def m; .scl 2; .type 32; .endef
  .globl m
  .p2align 4
m:
  retq

  .section .rdata,"dr"
  .globl _load_config_used
  .p2align 3
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .fill 80, 1, 0
