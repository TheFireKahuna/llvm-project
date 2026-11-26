# REQUIRES: x86

## A section whose loops a cache line is to keep goes at the next 16-byte place
## its loops would not cross a line from: the object's barred places for loops
## at its offsets, LLD's own for loops it lays out.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix %s -o %t.obj
# RUN: lld-link -dll -noentry -export:a -export:b -export:c -export:d \
# RUN:   -export:m -export:n -export:p %t.obj -out:%t.dll
# RUN: llvm-objdump -d --no-show-raw-insn %t.dll | FileCheck %s

## 12-byte loops: b's at 12, c's at 28, d's at 60 move b to 64, c to 112, d to
## 160; m's and n's loops follow a call written in five bytes for nine, and n's
## fourth loop takes 6 bytes of the object's 11 padding, its fifth none.
# CHECK:      180001000 <a>:
# CHECK:      180001040 <b>:
# CHECK:      180001056: jne 0x18000104c
# CHECK:      180001070 <c>:
# CHECK:      180001096: jne 0x18000108c
# CHECK:      1800010a0 <d>:
# CHECK:      1800010e6: jne 0x1800010dc
# CHECK:      1800010f0 <m>:
# CHECK-NEXT: 1800010f0: callq
# CHECK:      180001127: jne 0x18000111d
# CHECK:      180001130 <n>:
# CHECK:      18000116f: jne 0x180001165
# CHECK:      18000119f: jne 0x180001195
# CHECK:      1800011cf: jne 0x1800011c5
# CHECK:      1800011fa: nopw
# CHECK-NEXT: 180001200: nop
# CHECK:      18000120a: jne 0x180001200
# CHECK-NOT:  nop{{[lw]}}
# CHECK:      180001219: jne 0x18000120f

## p's second loop holds a third, which is kept with it: the second comes to
## lie at 57 and the third, at 66, within it.
# CHECK:      180001220 <p>:
# CHECK:      180001259: je
# CHECK-NOT:  nop{{[lw]}}
# CHECK:      18000126c: jne 0x180001262
# CHECK-NEXT: 18000126e: jmp 0x180001259

.macro loop name, before
  .rept \before
  nop
  .endr
  .p2align 4, , 10, .L\name\()_end
  .p2align 3, , , .L\name\()_end
.L\name:
  .rept 10
  nop
  .endr
  jne .L\name
.L\name\()_end:
.endm

.macro func name
  .section .text$\name,"xr"
  .globl \name
  .p2align 4
\name:
.endm

  func a
  .rept 47
  nop
  .endr
  ret

  func b
  loop b, 12
  ret

  func c
  loop c, 28
  ret

  func d
  loop d, 60
  ret

  .section .text$g,"xr"
  .globl g
g:
  ret

  func m
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  loop m, 40
  ret

  func n
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  loop n1, 48
  loop n2, 36
  loop n3, 36
  loop n4, 41
  loop n5, 3
  ret

  func p
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  loop p1, 40
  .p2align 0, , , .Lp2_end
.Lp2:
  je .Lp4
  .rept 3
  nop
  .endr
  loop p3, 0
  jmp .Lp2
.Lp2_end:
  .fill 200, 1, 0x90
.Lp4:
  ret
