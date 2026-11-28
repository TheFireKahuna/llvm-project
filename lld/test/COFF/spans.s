# REQUIRES: x86

## A section with stated spans is compacted (a bound load and call to a 5-byte
## call, a final jmp to a following target deleted, padding redone), and every
## field, difference, offset and unwind measure follows.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=x86_64-windows-itanium
# DEFINE: %{link} = lld-link -dll -noentry -nodefaultlib -debug:symtab
# RUN: %{mc} main.s -o main.obj
# RUN: %{mc} defs.s -o defs.obj
# RUN: %{mc} imp.s -o imp.obj
# RUN: %{mc} tiny.s -o tiny.obj
# RUN: %{link} imp.obj -export:h -out:imp.dll -implib:imp.lib
# RUN: %{link} main.obj defs.obj imp.lib -export:f -out:main.dll
# RUN: llvm-objdump -d main.dll | FileCheck --check-prefix=DIS %s
# RUN: llvm-objdump -s -j .rdata main.dll | FileCheck --check-prefix=DATA %s
# RUN: llvm-readobj --unwind main.dll | FileCheck --check-prefix=UNWIND %s

# DIS:      <f>:
# DIS-NEXT: 180001000: 48 83 ec 28 subq $0x28, %rsp
# DIS-NEXT: 180001004: 48 8d 05 15 00 00 00 leaq 0x15(%rip), %rax
# DIS-NEXT: 18000100b: 83 3d 0e 00 00 00 01 cmpl $0x1, 0xe(%rip)
# DIS-NEXT: 180001012: e8 {{.*}} callq {{.*}} <g>
# DIS-NEXT: 180001017: 75 f9 jne 0x180001012
# DIS-NEXT: 180001019: 0f 1f 80 00 00 00 00 nopl (%rax)
# DIS-NEXT: 180001020: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax
# DIS-NEXT: 180001027: ff d0 callq *%rax
# DIS-NEXT: 180001029: 75 f5 jne 0x180001020
# DIS-NEXT: 18000102b: 48 83 c4 28 addq $0x28, %rsp
# DIS-NEXT: 18000102f: c3 retq
## The table's entry is -0x1e, the distance of its target.
# DIS-NEXT: 180001030: e2 ff
# DIS:      180001034: e8 {{.*}} callq {{.*}} <g>
# DIS-NEXT: 180001039: 48 83 c4 28 addq $0x28, %rsp
# DIS-NEXT: 18000103d: e9 {{.*}} jmp {{.*}} <g>
## c, in another section, reaches the two points where they came to lie.
# DIS:      <c>:
# DIS-NEXT: e9 {{.*}} jmp 0x180001020
# DIS-NEXT: e8 {{.*}} callq 0x180001012
## k keeps its size: the distance in .rdata would not fit its byte once the
## padding grew, so the call has its no-operation.
# DIS:      <k>:
# DIS-NEXT: e8 {{.*}} callq {{.*}} <g>
# DIS-NEXT: 0f 1f 40 00 nopl
## t loses the rest of its pair and its jmp, and falls through to g.
# DIS:      <t>:
# DIS-NEXT: e8 {{.*}} callq {{.*}} <g>
# DIS-EMPTY:
# DIS-NEXT: <g>:

## Distances to the loop and the end, the loop's distance and address, k's as
## assembled, and t's length.
# DATA:      180002010 20000000 0e300042 08f0ffff 20100080
# DATA-NEXT: 180002020 01000000 20100000 7f050000 00

# UNWIND:      StartAddress: f (0x180001000)
# UNWIND-NEXT: EndAddress: {{.*}}(0x180001042)
# UNWIND:      EPILOG offset=0x13
# UNWIND-NEXT: EPILOG offset=0x5

## Equal bytes fold only where their spans measure equal code: xa's call became
## direct (5 bytes), xb's stays through its pointer (9).
# RUN: %{mc} icf.s -o icf.obj
# RUN: %{link} -opt:icf icf.obj defs.obj imp.lib -export:xa,data \
# RUN:   -export:xb,data -out:icf.dll
# RUN: llvm-objdump -s -j .rdata icf.dll | FileCheck --check-prefix=ICF %s

# ICF: 05090000 2b1d7a4f

## A span that names a point inside a rewrite, and a list that claims more
## spans than its group holds, are errors.
# RUN: obj2yaml tiny.obj | sed 's/01010600$/01010601/' | yaml2obj -o in.obj
# RUN: not %{link} in.obj defs.obj -out:in.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=IN %s
# RUN: obj2yaml tiny.obj | sed 's/01010600$/010F0600/' | yaml2obj -o bad.obj
# RUN: not %{link} bad.obj defs.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s

# IN: in.obj: .rdata+0x0: a span names no point of .text that the link keeps
# BAD: bad.obj: .llvm_link_records: 15 spans, which the group does not hold

## A JUMP32 is short where it reaches once compacted: ja's je but not its jne,
## one of jb's mutually dependent pair, jc's past grown padding, not jd's whose
## padding would grow out of reach, and je's to g2 in the next chunk.
# RUN: %{mc} jumps.s -o jumps.obj
# RUN: %{mc} next.s -o next.obj
# RUN: %{link} jumps.obj next.obj defs.obj imp.lib -out:jumps.dll
# RUN: llvm-objdump -d jumps.dll | FileCheck --check-prefix=JUMP %s

# JUMP:      <ja>:
# JUMP-NEXT: 180001000: 74 7f je 0x180001081
# JUMP-NEXT: e8 {{.*}} callq {{.*}} <g>
# JUMP:      180001081: 0f 85 83 00 00 00 jne 0x18000110a
# JUMP-NEXT: 48 8b 05 {{.*}} movq
# JUMP:      <jb>:
# JUMP-NEXT: 18000110b: e9 80 00 00 00 jmp 0x180001190
# JUMP:      180001189: 74 80 je 0x18000110b
# JUMP:      180001190: 75 bd jne 0x18000114f
# JUMP:      <jc>:
# JUMP-NEXT: e8 {{.*}} callq {{.*}} <g>
# JUMP-NEXT: 1800011a5: eb 79 jmp 0x180001220
# JUMP:      180001219: 0f 1f 80 00 00 00 00 nopl
# JUMP-NEXT: 180001220: eb 6e jmp 0x180001290
# JUMP:      <jd>:
# JUMP-NEXT: e8 {{.*}} callq {{.*}} <g>
# JUMP-NEXT: 1800012a5: e9 7e 00 00 00 jmp 0x180001328
# JUMP-NEXT: 1800012aa: 66 0f 1f 44 00 00 nopw
# JUMP:      <je>:
# JUMP-NEXT: 180001329: 90 nop
# JUMP-NEXT: 18000132a: eb 04 jmp 0x180001330 <g2>
# JUMP:      <g2>:
# JUMP-NEXT: 180001330: c3 retq

## A JUMP32 names a jmp or jcc.
# RUN: obj2yaml jumps.obj | sed 's/A401F00E/A401F40E/' | yaml2obj -o nojump.obj
# RUN: not %{link} nojump.obj next.obj defs.obj imp.lib -out:nojump.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=NOJUMP %s

# NOJUMP: nojump.obj: .text$jc+0x81: a span states no jump within the section

#--- main.s
  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
.Lbegin:
  .seh_proc f
  .seh_unwindversion 2
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  leaq .Lloop(%rip), %rax
  cmpl $1, .Lloop(%rip)
.Lcall:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  jne .Lcall
  .p2align 4
.Lloop:
  movq __imp_h@LOAD_CALL(%rip), %rax
  callq *%rax
  jne .Lloop
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
.Ltable:
  .long .Lcall - .Ltable
  movq __imp_g@LOAD_CALL(%rip), %r11
  callq *%r11
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  jmp g
.Lend:
  .seh_endproc

  .section .text$c,"xr"
  .globl c
c:
  jmp .Lloop
  callq .Lcall

  .section .text$k,"xr"
  .globl k
k:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
.Lk:
  .fill 126, 1, 0x90
  .p2align 3
.Lkend:
  retq

  .section .text$y,"xr"
  .globl t
t:
.Lt:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  jmp g
.Ltend:

  .section .rdata,"dr"
  .long .Lloop - .Lbegin
  .byte .Lloop - .Lcall
  .short .Lend - .Lcall
  .uleb128 .Lend - .Lbegin
  .long .Lloop - .
  .quad .Lloop
  .long .Lloop@IMGREL
  .byte .Lkend - .Lk
  .byte .Ltend - .Lt

#--- fn.inc
.macro fn name, sect
  .section \sect,"xr"
  .globl \name
\name:
.endm

#--- jumps.s
  .include "fn.inc"
  fn ja, .text$ja
  je .La1
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  .fill 122, 1, 0x90
.La1:
  jne .La2
  movq __imp_h@LOAD_CALL(%rip), %rax
  callq *%rax
  .fill 122, 1, 0x90
.La2:
  retq

  fn jb, .text$jb
.Lb0:
  jmp .Lb2
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  .fill 58, 1, 0x90
.Lb1:
  .fill 58, 1, 0x90
  je .Lb0
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
.Lb2:
  jne .Lb1
  retq

  fn jc, .text$jc
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  jmp .Lc1
  .fill 114, 1, 0x90
  .p2align 4
.Lc1:
  jmp .Lc2
  .fill 109, 1, 0x90
  .p2align 4
.Lc2:
  retq

  fn jd, .text$jd
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  jmp .Ld1
  .p2align 4
  .fill 120, 1, 0x90
.Ld1:
  retq

  fn je, .text$je
  nop
  jmp g2

#--- next.s
  .section .text$jf,"xr"
  .long 0x12345678
  .globl g2
g2:
  retq

#--- defs.s
  .section .text$z,"xr"
  .globl g
  .def g; .scl 2; .type 32; .endef
g:
  retq

#--- imp.s
  .text
  .globl h
  .def h; .scl 2; .type 32; .endef
h:
  retq

#--- icf.s
  .section .text$a,"xr",discard,fa
  .globl fa
fa:
.La0:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
.La1:
  retq
  .section .text$b,"xr",discard,fb
  .globl fb
fb:
.Lb0:
  movq __imp_h@LOAD_CALL(%rip), %rax
  callq *%rax
.Lb1:
  retq
  .section .xdata$a,"dr",discard,xa
  .globl xa
xa:
  .byte .La1 - .La0
  .section .xdata$b,"dr",discard,xb
  .globl xb
xb:
  .byte .Lb1 - .Lb0

#--- tiny.s
  .text
.Lb:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
.Le:
  retq
  .section .rdata,"dr"
  .byte .Le - .Lb
