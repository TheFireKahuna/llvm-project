# REQUIRES: x86, aarch64

## Fused add, access and test sites bind by their pointer where the assembler
## checked them: a local import's is rewritten to reach the target, folding an
## ARM64 page pair where it reaches; any other pointer's site stays as compiled.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab
# DEFINE: %{dump} = llvm-objdump -d

# RUN: %{mc} -triple=x86_64-windows-itanium -mattr=+egpr x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium defs.s -o defs-x64.obj
# RUN: lld-link -dll -noentry -export:f -export:v,data -export:u,data \
# RUN:   defs-x64.obj -out:d-x64.dll -implib:d-x64.lib
# RUN: %{link} -out:x64.exe x64.obj defs-x64.obj 2>&1 | count 0
# RUN: %{dump} x64.exe | FileCheck --check-prefix=X64 %s
# RUN: llvm-readobj --sections x64.exe | FileCheck --check-prefix=SPAN %s
# RUN: %{link} -out:x64-imp.exe x64.obj d-x64.lib
# RUN: %{dump} x64-imp.exe | FileCheck --check-prefix=X64-IMP %s

## Reads whose accesses all reach v go; a copied one and a load another
## instruction reads stay; an absent symbol's pointer is zero, the lea adds to it.
# X64-LABEL: <main>:
# X64-NEXT:    8b 0d {{.*}} movl {{.*}} # 0x[[#%x,V:]] <v>
# X64-NEXT:    f0 lock
# X64-NEXT:    ff 05 {{.*}} incl {{.*}} <v+0x4>
# X64-NEXT:    83 3d {{.*}} 05 cmpl $0x5, {{.*}} <v>
# X64-NEXT:    movq %r12, %r13
# X64-NEXT:    c5 fc 10 0d {{.*}} vmovups {{.*}} # 0x[[#%x,V + 0x12345]]
# X64-NEXT:    leaq {{.*}} <v>
# X64-NEXT:    89 0d {{.*}} movl %ecx, {{.*}} <v>
# X64-NEXT:    movq %rax, %rdx
# X64-NEXT:    callq {{.*}} <f>
# X64-NEXT:    48 8d 05 {{.*}} leaq {{.*}} <v+0x4>
# X64-NEXT:    4c 8d 25 {{.*}} leaq {{.*}} # 0x[[#%x,V - 16]]
# X64-NEXT:    movq $0x0, %rcx
# X64-NEXT:    leaq 0x8(%rcx), %rcx
# X64-NEXT:    leaq {{.*}} <u>
# X64-NEXT:    movl (%rax), %ecx
# X64-NEXT:    retq
# X64-LABEL: <same>:
# X64-NEXT:    4c 8d 25 {{.*}} leaq {{.*}} <v+0x4>
# X64-NEXT:    0f 1f 44 00 00 nopl (%rax,%rax)
# X64-NEXT:    leaq {{.*}} <v>
# X64-NEXT:    movl (%rax), %ecx
# X64-NEXT:    movq $0x0, %rcx
# X64-NEXT:    testq %rcx, %rcx
## A dead read, its test and its falling-through je; two compares with taken
## jumps, a 32-bit one as jmp and int3.
# X64-NEXT:    0f 1f 80 00 00 00 00 nopl
# X64-NEXT:    0f 1f 00 nopl
# X64-NEXT:    66 90 nop
# X64-NEXT:    : 90 nop
# X64-NEXT:    0f 1f 84 00 00 00 00 00 nopl
# X64-NEXT:    e9 {{.*}} jmp {{.*}} <main>
# X64-NEXT:    : cc int3
# X64-NEXT:    0f 1f 84 00 00 00 00 00 nopl
# X64-NEXT:    eb {{.*}} jmp {{.*}} <same+0x2c>
# X64-NEXT:    retq
## The jump after the compare there keeps its eight bits.
# X64-LABEL: <keep>:
# X64-NEXT:    leaq {{.*}} <v>
# X64-NEXT:    movl (%rax), %ecx
# X64:         0f 1f 84 00 00 00 00 00 nopl
# X64-NEXT:    eb 01 jmp
# X64-NEXT:    nop
# X64-NEXT:    retq
# X64-LABEL: <long>:
# X64-NEXT:    0f 1f 84 00 00 00 00 00 nopl
# X64-NEXT:    e9 81 00 00 00 jmp {{.*}} <long+0x8e>
# X64-NEXT:    : cc int3
## v present, w absent: v's test, je and read go, its compare's jne is a jmp;
## w's je is a jmp, its jne goes, and its call's load is a mov of zero.
# X64-LABEL: <test>:
# X64-NEXT:    movl {{.*}} <v>
# X64-NEXT:    e9 {{.*}} jmp {{.*}} <main>
# X64-NEXT:    eb 09 jmp
# X64-NEXT:    movq $0x0, %rdx
# X64-NEXT:    callq *%rdx
# X64-NEXT:    retq
## A mov reads the flags that the fourth load leaves alone, and the last
## pointer holds an absolute address.
# X64-LABEL: <zero>:
# X64-NEXT:    31 c9 xorl %ecx, %ecx
# X64-NEXT:    testq %rcx, %rcx
# X64-NEXT:    45 31 c9 xorl %r9d, %r9d
# X64-NEXT:    cmpq $0x1, %r9
# X64-NEXT:    d5 55 31 c9 xorl %r25d, %r25d
# X64-NEXT:    testq %r25, %r25
# X64-NEXT:    movq $0x0, %rdx
# X64-NEXT:    movq %rdx, %rax
# X64-NEXT:    movq $0x1234, %rcx

## The image holds no pointer to v or to f: its read-only data is the byte
## of the span and its empty offer table, and on ARM64 the table alone.
# SPAN:       Name: .rdata
# SPAN-NEXT:  VirtualSize: 0x20
# NOPTR:      Name: .rdata
# NOPTR-NEXT: VirtualSize: 0x18

# X64-IMP-LABEL: <main>:
# X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
# X64-IMP-NEXT:    movl (%rax), %ecx
# X64-IMP:         movq {{.*}}(%rip), %rdx
# X64-IMP-NEXT:    callq *%rdx
# X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
# X64-IMP-NEXT:    leaq 0x4(%rax), %rax
# X64-IMP:         testq %rax, %rax
# X64-IMP-NEXT:    je
# X64-IMP-LABEL: <test>:
# X64-IMP:         cmpq $0x0, {{.*}}(%rip)
# X64-IMP-NEXT:    jne {{.*}} <main>

## The sites of an object whose assembler did not check them stay, and its
## sections fold only with those that bind alike: a and b, and not c.
# RUN: %{mc} -triple=x86_64-windows-itanium icf.s -o icf.obj
# RUN: %{mc} -triple=x86_64-windows-itanium icf-c.s -o - | obj2yaml | \
# RUN:   sed 's/SectionData:     4C4C52430108/SectionData:     4C4C52430100/' | \
# RUN:   yaml2obj -o icf-unchecked.obj
# RUN: %{link} -opt:icf -verbose -out:icf.exe icf.obj icf-unchecked.obj \
# RUN:   defs-x64.obj 2>&1 | FileCheck --check-prefix=ICF %s
# RUN: %{dump} icf.exe | FileCheck --check-prefix=UNCHECKED %s

# ICF:      Selected a
# ICF-NEXT:   Removed b
# ICF-NOT:  Removed

# UNCHECKED-LABEL: <c>:
# UNCHECKED-NEXT:    leaq {{.*}} <v>
# UNCHECKED-NEXT:    leaq 0x4(%rax), %rax
# UNCHECKED-NEXT:    cmpq $0x0, {{.*}}(%rip)
# UNCHECKED-NEXT:    jne

## Reported: leas whose index states another constant, register or length or
## that follow a load without a relocation; an access that reads its register
## again; a test turned into a copy, one followed by jb, and a compare with one.
# RUN: %{mc} -triple=x86_64-windows-itanium bad.s -o - | obj2yaml | \
# RUN:   sed -e 's/Index: 132/Index: 164/' -e 's/488D4008/488D4908/' \
# RUN:     -e 's/Index: 388/Index: 389/' \
# RUN:     -e 's/488D4014488B0500000000/488B0500000000488D4014/' \
# RUN:     -e 's/VirtualAddress:  40$/VirtualAddress:  47/' \
# RUN:     -e 's/8B4804/894004/' -e 's/4885D2/4889D1/' \
# RUN:     -e 's/4885C974/4885C972/' -e 's/3D0000000000/3D0000000001/' | \
# RUN:   yaml2obj -o bad.obj
# RUN: not %{link} -out:bad.exe bad.obj defs-x64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD --implicit-check-not=error: %s

# BAD:      error: bad.obj: .text+0x7: IMAGE_REL_LLVM_FUSED_ADD
# BAD-NEXT: error: bad.obj: .text+0x12: IMAGE_REL_LLVM_FUSED_ADD
# BAD-NEXT: error: bad.obj: .text+0x1d: IMAGE_REL_LLVM_FUSED_ADD
# BAD-NEXT: error: bad.obj: .text+0x2f: IMAGE_REL_LLVM_FUSED_ADD
# BAD-NEXT: error: bad.obj: .text+0x3a: IMAGE_REL_LLVM_FUSED_ACCESS
# BAD-NEXT: error: bad.obj: .text+0x44: IMAGE_REL_LLVM_FUSED_TEST_REG
# BAD-NEXT: error: bad.obj: .text+0x50: IMAGE_REL_LLVM_FUSED_TEST_REG
# BAD-NEXT: error: bad.obj: .text+0x58: IMAGE_REL_LLVM_FUSED_TEST

# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium defs.s -o defs-a64.obj
# RUN: lld-link -dll -noentry -export:f -export:v,data -export:u,data \
# RUN:   -export:q,data -export:e,data -export:far,data defs-a64.obj \
# RUN:   -out:d-a64.dll -implib:d-a64.lib
# RUN: %{link} -out:a64.exe a64.obj defs-a64.obj 2>&1 | count 0
# RUN: %{dump} a64.exe | FileCheck --check-prefix=A64 %s
# RUN: llvm-readobj --sections a64.exe | FileCheck --check-prefix=NOPTR %s
# RUN: %{link} -out:a64-imp.exe a64.obj d-a64.lib
# RUN: %{dump} a64-imp.exe | FileCheck --check-prefix=A64-IMP %s

## u is 12 bytes into its page, q 16, e 0xffc and far 0xff8.
## v is there: the cbz is gone, and the read with it.
# A64-LABEL: <main>:
# A64-NEXT:    ldr w9, 0x[[#%x,]] <v>
# A64-NEXT:    bl 0x[[#%x,]] <f>
# A64-NEXT:    adr x0, 0x[[#%x,]] <q>
# A64-NEXT:    ret
# A64-LABEL: <access>:
# A64-NEXT:    adrp x8, 0x[[#%x,]]
# A64-NEXT:    ldr w9, [x8, #0x10]
# A64-NEXT:    str x9, [x8, #0x18]
# A64-NEXT:    ldp x1, x2, [x8, #0x20]
# A64-NEXT:    ret
# A64-LABEL: <misfit>:
# A64-NEXT:    adrp x8, 0x[[#%x,]]
# A64-NEXT:    add x8, x8, #0xffc
# A64-NEXT:    add x0, x8, #0x8
# A64-NEXT:    add x1, x8, #0x0
# A64-NEXT:    ret
# A64-LABEL: <reach>:
# A64-NEXT:    adrp x8, 0x[[#%x,]] <far+0x8>
# A64-NEXT:    add x0, x8, #0x0
# A64-NEXT:    ret
# A64-LABEL: <misaligned>:
# A64-NEXT:    adrp x8, 0x[[#%x,]]
# A64-NEXT:    add x8, x8, #0x4
# A64-NEXT:    str x9, [x8]
# A64-NEXT:    ret
# A64-LABEL: <noread>:
# A64-NEXT:    add x0, x8, #0x4
## A load of a local import pointer that no site reads is the adr of the
## pointer's target, as a fused read with a site that stays is.
# A64-LABEL: <other>:
# A64-NEXT:    adr x8, 0x[[#%x,]] <u>
# A64-NEXT:    adr x8, 0x[[#%x,]] <u>
# A64-NEXT:    add x0, x8, #0x4
# A64-NEXT:    ret
# A64-LABEL: <pair>:
# A64-NEXT:    adrp x8, 0x[[#%x,]]
# A64-NEXT:    add x8, x8, #0xffc
# A64-NEXT:    ldp w1, w2, [x8]
# A64-NEXT:    ret
# A64-LABEL: <same>:
# A64-NEXT:    adrp x8, 0x[[#%x,]]
# A64-NEXT:    nop
# A64-NEXT:    add x0, x8, #0x10
## A cbnz with no relocation, which keeps its distance to the no-operation
## before it, and one that is not taken.
# A64-NEXT:    nop
# A64-NEXT:    nop
# A64-NEXT:    nop
# A64-NEXT:    b 0x[[#%x,]] <same+0x14>
# A64-NEXT:    nop
# A64-NEXT:    ret
## w is not there: its cbz is a b, its cbnz is gone, and so are its reads.
# A64-LABEL: <test>:
# A64-NEXT:    b 0x[[#%x,]] <main>
# A64-NEXT:    ret
# A64-LABEL: <own>:
# A64-NEXT:    adrp x8, 0x[[#%x,]] <e+0x4>
# A64-NEXT:    str w0, [x8]
# A64-NEXT:    ret

# A64-IMP-LABEL: <main>:
# A64-IMP-NEXT:    ldr x8, 0x[[#%x,]] <__imp_v>
# A64-IMP-NEXT:    ldr w9, [x8]
# A64-IMP-NEXT:    cbz x8, 0x[[#%x,]]
# A64-IMP-NEXT:    ldr x10, 0x[[#%x,]] <__imp_f>
# A64-IMP-NEXT:    blr x10
# A64-IMP-NEXT:    ldr x8, 0x[[#%x,]] <__imp_u>
# A64-IMP-NEXT:    add x0, x8, #0x4
# A64-IMP-NEXT:    ret
# A64-IMP-LABEL: <test>:
# A64-IMP-NEXT:    b 0x[[#%x,]] <main>
# A64-IMP-NEXT:    ret

## Reported: a 32-bit add, a load writing its base back, a 32-bit cbz.
# RUN: %{mc} -triple=aarch64-windows-itanium bad-a64.s -o - | obj2yaml | \
# RUN:   sed -e 's/00110091/00110011/' -e 's/090140B9/090C40B8/' \
# RUN:     -e 's/080000B4/08000034/' | yaml2obj -o bad-a64.obj
# RUN: not %{link} -out:bad-a64.exe bad-a64.obj defs-a64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD-A64 --implicit-check-not=error: %s

# BAD-A64:      error: bad-a64.obj: .text+0x8: IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL
# BAD-A64-NEXT: error: bad-a64.obj: .text+0xc: IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL
# BAD-A64-NEXT: error: bad-a64.obj: .text+0x10: IMAGE_REL_LLVM_FUSED_TEST

#--- x64.s
  .text
  .globl main
main:
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax), %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  lock incl 4(%rax)
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  cmpl $5, (%rax)
  movq __imp_v@FUSED(%rip), %r12
  movq %r12, %r13
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  vmovups 0x12345(%r13), %ymm1
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl %ecx, (%rax)
  movq %rax, %rdx
  movq __imp_f@LOAD_CALL(%rip), %rdx
  callq *%rdx
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 4(%rax), %rax
  movq __imp_v(%rip), %r12
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq -16(%r12), %r12
  movq __imp_w(%rip), %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 8(%rcx), %rcx
  movq __imp_u(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_u
  testq %rax, %rax
  je 1f
  movl (%rax), %ecx
1:
  retq
  .weak w

## The fill is data, so the link does not resize this section.
  .section .text$d,"xr"
  .globl same
same:
  movq __imp_v(%rip), %r12
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 4(%r12), %r12
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax), %ecx
  movq __imp_w(%rip), %rcx
  testq %rcx, %rcx
  movq __imp_u@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_u
  testq %rax, %rax
  je 1f
  nop
1:
  cmpq $0, __imp_v@FUSED(%rip)
  jne main
  cmpq $0, __imp_w@FUSED(%rip)
  je 1b
  retq
  .p2align 2, 0

## A jump that the assembler resolved, of 32 bits.
  .section .text$l,"xr"
  .globl long
long:
  cmpq $0, __imp_v@FUSED(%rip)
  jne 1f
  .fill 128, 1, 0x90
1:
  retq
  .p2align 2, 0

  .section .text$t,"xr"
  .globl test
test:
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rax, %rax
  je 1f
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax), %ecx
1:
  cmpq $0, __imp_v@FUSED(%rip)
  jne main
  movq __imp_w@FUSED(%rip), %rdx
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_w
  testq %rdx, %rdx
  je 2f
  movq __imp_w@LOAD_CALL(%rip), %rdx
  callq *%rdx
2:
  cmpq $0, __imp_w@FUSED(%rip)
  jne 1b
  retq

  .section .text$z,"xr"
  .globl zero
zero:
  movq __imp_w(%rip), %rcx
  testq %rcx, %rcx
  movq __imp_w(%rip), %r9
  cmpq $1, %r9
  movq __imp_w(%rip), %r25
  testq %r25, %r25
  movq __imp_w(%rip), %rdx
  movq %rdx, %rax
  movq __imp_a(%rip), %rcx
  testq %rcx, %rcx
  retq
  movq __imp_v@FUSED(%rip), %rax
  .globl a
a = 0x1234

## A span over the access does not hold its length once the access is
## longer, so the section keeps its size.
  .section .text$k,"xr"
  .globl keep
keep:
  movq __imp_v@FUSED(%rip), %rax
.La:
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax), %ecx
  .fill 124, 1, 0x90
.Lb:
  cmpq $0, __imp_w@FUSED(%rip)
  je .Lc
  nop
.Lc:
  retq
  .section .rdata,"dr"
  .byte .Lb - .La

#--- fold.inc
.macro fn name
  .section .text$fold,"xr",discard,\name
  .globl \name
\name:
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 4(%rax), %rax
  cmpq $0, __imp_v@FUSED(%rip)
  jne \name
  ret
.endm

#--- icf.s
  .include "fold.inc"
  .globl main
main:
  callq a
  callq b
  callq c
  ret
fn a
fn b
  .addrsig

#--- icf-c.s
  .include "fold.inc"
fn c
  .addrsig

#--- bad.s
  .text
  .globl main
main:
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 4(%rax), %rax
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 8(%rax), %rax
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 12(%rax), %rax
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 20(%rax), %rax
  movq 0(%rip), %rax
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl 4(%rax), %ecx
  movq __imp_v@FUSED(%rip), %rdx
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rdx, %rdx
  je 1f
1:
  movq __imp_v@FUSED(%rip), %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rcx, %rcx
  je 1f
1:
  cmpq $0, __imp_v@FUSED(%rip)
  je 1b

#--- a64.s
.macro fn name, sect
  .section \sect,"xr"
  .globl \name
\name:
.endm
  .text
  .globl main
main:
  adrp x8, __imp_v
  ldr x8, [x8, :fused_lo12:__imp_v]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  ldr w9, [x8]
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  cbz x8, 1f
1:
  adrp x10, __imp_f
  ldr x10, [x10, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x10
  adrp x8, __imp_u
  ldr x8, [x8, :fused_lo12:__imp_u]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_u
  add x0, x8, #4
  ret

  fn access, .text$a
  adrp x8, __imp_q
  ldr x8, [x8, :fused_lo12:__imp_q]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_q
  ldr w9, [x8]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_q
  str x9, [x8, #8]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_q
  ldp x1, x2, [x8, #16]
  ret

## The sum is no multiple of the store's size.
  fn misaligned, .text$m
  adrp x8, __imp_v
  ldr x8, [x8, :fused_lo12:__imp_v]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  str x9, [x8]
  ret

## A store has no literal form, and what it reaches is in the page after e's.
  fn own, .text$w
  adrp x8, __imp_e
  ldr x8, [x8, :fused_lo12:__imp_e]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_e
  str w0, [x8, #4]
  ret

## An adr does not reach far.
  fn reach, .text$f
  adrp x8, __imp_far
  ldr x8, [x8, :fused_lo12:__imp_far]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_far
  add x0, x8, #8
  ret

## The sum does not fit the pair.
  fn pair, .text$p
  adrp x8, __imp_e
  ldr x8, [x8, :fused_lo12:__imp_e]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_e
  ldp w1, w2, [x8]
  ret

## The sum does not fit the first add.
  fn misfit, .text$e
  adrp x8, __imp_e
  ldr x8, [x8, :fused_lo12:__imp_e]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_e
  add x0, x8, #8
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_e
  add x1, x8, #0
  ret

## No load of the pointer is in the add's section.
  fn noread, .text$n
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_u
  add x0, x8, #4
  ret

## Another load of the pointer may reach the add.
  fn other, .text$o
  adrp x8, __imp_u
  ldr x8, [x8, :lo12:__imp_u]
  adrp x8, __imp_u
  ldr x8, [x8, :fused_lo12:__imp_u]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_u
  add x0, x8, #4
  ret

## The fill is data, so the link does not resize this section.
  fn same, .text$s
  adrp x8, __imp_u
  ldr x8, [x8, :fused_lo12:__imp_u]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_u
  add x0, x8, #4
  adrp x9, __imp_v
  ldr x9, [x9, :fused_lo12:__imp_v]
1:
  nop
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  cbnz x9, 1b
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  cbz x9, main
  ret
  .p2align 3, 0

  fn test, .text$t
  adrp x10, __imp_w
  ldr x10, [x10, :fused_lo12:__imp_w]
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_w
  cbz x10, main
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_w
  cbnz x10, main
  ret
  .weak w

#--- bad-a64.s
  .text
  .globl main
main:
  adrp x8, __imp_u
  ldr x8, [x8, :fused_lo12:__imp_u]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_u
  add x0, x8, #4
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_u
  ldr w9, [x8]
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_u
  cbz x8, .

#--- defs.s
  .text
  .globl f
f:
  ret
  .data
  .globl v, u, q, e
  .long 7
v:
  .long 1, 2
u:
  .long 3
q:
  .quad 5
## The last word of its page.
  .space 0xfe4
e:
  .long 4
  .bss
  .space 0x100ff8
  .globl far
far:
  .space 16
