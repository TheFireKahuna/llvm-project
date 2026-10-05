# REQUIRES: x86

## A required pin the linker cannot honour is an error; another is a warning,
## and the section is placed without it. Pins that disagree are reported with
## both objects, and the outcome does not depend on the order of the inputs.

# RUN: split-file %s %t
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/a.s -filetype=obj -o %t/a.obj
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/b.s -filetype=obj -o %t/b.obj
# RUN: not lld-link %t/a.obj %t/b.obj -entry:main -opt:noref -out:%t/out.exe 2>&1 | \
# RUN:   FileCheck %s
# RUN: not lld-link %t/b.obj %t/a.obj -entry:main -opt:noref -out:%t/out.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=SWAP %s
# RUN: llvm-mc -triple x86_64-unknown-windows-itanium %t/c.s -filetype=obj -o %t/c.obj
# RUN: not lld-link %t/c.obj -entry:main -base:0x140000800 \
# RUN:   -out:%t/out.exe 2>&1 | FileCheck --check-prefix=BASE %s

# CHECK-DAG: error: {{.*}}b.obj: pin of required conflicts with the pin of required in {{.*}}a.obj
# CHECK-DAG: warning: {{.*}}b.obj: pin of advisory conflicts with the pin of advisory in {{.*}}a.obj; the pins that are not required are left out
# CHECK-DAG: error: {{.*}}a.obj: pin of aligned conflicts with the alignment of its section
# CHECK-DAG: warning: {{.*}}a.obj: pin of aligned_advisory conflicts with the alignment of its section
# CHECK-DAG: error: {{.*}}a.obj: pin of wide asks for a modulus larger than a page
# SWAP-DAG: error: {{.*}}a.obj: pin of required conflicts with the pin of required in {{.*}}b.obj
# SWAP-DAG: warning: {{.*}}a.obj: pin of advisory conflicts with the pin of advisory in {{.*}}b.obj; the pins that are not required are left out
# BASE: error: /base: an image with pinned sections must be based at a multiple of 4096

#--- a.s
  .text
  .globl main
main:
  retq

  .section .rdata,"dr",discard,required
  .p2align 3
  .globl required
required:
  .quad 0
  .linkpin required, 12, 8, required

  .section .rdata,"dr",discard,advisory
  .p2align 3
  .globl advisory
advisory:
  .quad 0
  .linkpin advisory, 6, 8

  .section .rdata,"dr",discard,aligned
  .p2align 6
  .globl aligned
aligned:
  .quad 0
  .linkpin aligned, 12, 8, required

  .section .rdata,"dr",discard,aligned_advisory
  .p2align 6
  .globl aligned_advisory
aligned_advisory:
  .quad 0
  .linkpin aligned_advisory, 6, 8

  .section .rdata,"dr",discard,wide
  .p2align 3
  .globl wide
wide:
  .quad 0
  .linkpin wide, 13, 8, required

#--- b.s
  .section .rdata,"dr",discard,required
  .p2align 3
  .globl required
required:
  .quad 0
  .linkpin required, 12, 16, required

  .section .rdata,"dr",discard,advisory
  .p2align 3
  .globl advisory
advisory:
  .quad 0
  .linkpin advisory, 6, 16

#--- c.s
  .text
  .globl main
main:
  retq
  .section .rdata,"dr"
  .p2align 3
pinned:
  .quad 0
  .linkpin pinned, 12, 8, required
