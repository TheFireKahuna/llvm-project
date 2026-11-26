# REQUIRES: x86, aarch64

## In resizable code jumps start short and lengthen only out of reach, a jcc
## over a jmp becomes the opposite jcc, boundary padding follows its jump, and
## ARM64 address pairs, of a symbol plus the addend both fields state, fold to
## adr or a literal load while in reach, and fold again, once, where a layout
## that unfolded them leaves them in reach.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bb.s -o bb.obj \
# RUN:   --x86-align-branch-boundary=32 --x86-align-branch=jcc
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64.obj

# RUN: lld-link -dll -noentry -export:a -debug -debugtype:fixup -pdb:x64.pdb \
# RUN:   x64.obj bb.obj -out:x64.dll -call-graph-profile-sort:none
# RUN: llvm-objdump -d x64.dll | FileCheck --check-prefix=X64 %s
# RUN: llvm-pdbutil dump -fixups x64.pdb | FileCheck --check-prefix=X64-FIXUP %s
# RUN: lld-link -machine:arm64 -dll -noentry -export:a -debug -debugtype:fixup \
# RUN:   -pdb:a64.pdb a64.obj -out:a64.dll
# RUN: llvm-objdump -d --no-show-raw-insn a64.dll | FileCheck --check-prefix=A64 %s
# RUN: llvm-pdbutil dump -fixups a64.pdb | FileCheck --check-prefix=A64-FIXUP %s

## a: the jne and its jmp are a je; the je and jmp to g are short and the jmp
## to far is not; the other three jne keep their jmp.
# X64:      <a>:
# X64-NEXT: 180001000: 74 01 je 0x180001003
# X64-NEXT: 180001002: 90 nop
# X64-NEXT: 180001003: 74 3a je 0x18000103f
# X64-NEXT: 180001005: eb 38 jmp 0x18000103f
# X64-NEXT: 180001007: e9 89 01 00 00 jmp 0x180001195
# X64-NEXT: 18000100c: 75 02 jne 0x180001010
# X64-NEXT: 18000100e: eb 2f jmp 0x18000103f
# X64-NEXT: 180001010: 75 02 jne 0x180001014
# X64-NEXT: 180001012: eb ef jmp 0x180001003
# X64-NEXT: 180001014: 75 ed jne 0x180001003
# X64-NEXT: 180001016: eb ea jmp 0x180001002
# X64-NEXT: 180001018: c3 retq
## bb: no padding before the je, which ends before the boundary.
# X64:      18000103b: 90 nop
# X64-NEXT: 18000103c: 74 01 je 0x18000103f
# X64-NEXT: 18000103e: c3 retq
## g, then c: one je, which took 32 bits after the jmp to far did.
# X64-NEXT: 18000103f: c3 retq
# X64-NEXT: 180001040: 0f 84 82 00 00 00 je 0x1800010c8
# X64-NEXT: 180001046: 90 nop
# X64-NEXT: 180001047: e9 49 01 00 00 jmp 0x180001195
# X64:      1800010c8: c3 retq
## d: one je, though .Ldbg was at the jmp.
# X64-NEXT: 1800010c9: 74 01 je 0x1800010cc
# X64-NEXT: 1800010cb: 90 nop
# X64-NEXT: 1800010cc: c3 retq

## A jump is recorded as JUMP8 where it is short, a je made of a jne and a jmp
## with the jmp's target, and a JUMP8 that took 32 bits as JUMP32.
# X64-FIXUP:      8022 | 4000  | 00001001 | 00001003
# X64-FIXUP-NEXT: 8022 | 0000  | 00001004 | 0000103F
# X64-FIXUP-NEXT: 8022 | 0000  | 00001006 | 0000103F
# X64-FIXUP-NEXT: 8002 | 0000  | 00001008 | 00001195
# X64-FIXUP-NEXT: 8022 | 4000  | 0000100D | 00001010
# X64-FIXUP-NEXT: 8022 | 0000  | 0000100F | 0000103F
# X64-FIXUP-NEXT: 8022 | 4000  | 00001011 | 00001014
# X64-FIXUP-NEXT: 8022 | 4000  | 00001013 | 00001003
# X64-FIXUP-NEXT: 8022 | 4000  | 00001015 | 00001003
# X64-FIXUP-NEXT: 8022 | 4000  | 00001017 | 00001002
# X64-FIXUP-NEXT: 8022 | 0000  | 0000103D | 0000103F
# X64-FIXUP-NEXT: 8021 | 4000  | 00001042 | 000010C8
# X64-FIXUP-NEXT: 8002 | 0000  | 00001048 | 00001195
# X64-FIXUP-NEXT: 8022 | 4000  | 000010CA | 000010CC
# X64-FIXUP-NEXT: 0001 | 4000  | 00003000 | 00001012
## .Ldbg lies after the je.
# X64-FIXUP-NEXT: 0001 | 4000  | 00005000 | 000010CB

# A64:      180001000:   adr x0, 0x180003000
# A64-NEXT: 180001004:   ldr x1, 0x180003000
# A64-NEXT: 180001008:   ldrsw x2, 0x180003000
# A64-NEXT: 18000100c:   adrp x3, 0x180003000
# A64-NEXT: 180001010:   add x4, x3, #0x0
# A64-NEXT: 180001014:   adrp x5, 0x180003000
# A64-NEXT: 180001018:   add x5, x5, #0x0
# A64-NEXT: 18000101c:   adrp x5, 0x180003000
# A64-NEXT: 180001020:   add x5, x5, #0x8
# A64-NEXT: 180001024:   adrp x5, 0x180003000
# A64-NEXT: 180001028:   ldr x5, [x5, #0x8]
# A64-NEXT: 18000102c:   adrp x5, 0x180003000
# A64-NEXT: 180001030:   add x5, x5, #0x8
# A64-NEXT: 180001034:   adrp x6, 0x180003000
# A64-NEXT: 180001038:   ldrb w6, [x6]
# A64-NEXT: 18000103c:   adrp x7, 0x180003000
# A64-NEXT: 180001040:   add x7, x7, #0x0
# A64-NEXT: 180001044:   cbz x0, 0x180001040
# A64-NEXT: 180001048:   adrp x8, 0x180203000
# A64-NEXT: 18000104c:   add x8, x8, #0x10
# A64-NEXT: 180001050:   adrp x9, 0x180203000
# A64-NEXT: 180001054:   ldr x9, [x9, #0x10]
# A64-NEXT: 180001058:   adr x10, 0x180003000
# A64-NEXT: 18000105c:   adr x11, 0x180003008
# A64-NEXT: 180001060:   ldr x12, 0x180003000
# A64-NEXT: 180001064:   adrp x13, 0x180203000
# A64-NEXT: 180001068:   add x13, x13, #0x18
# A64-NEXT: 18000106c:   ret

## The first pair's target is out of reach. The second's is in reach only once
## the first unfolds, which moves it on towards data that stays.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium refold.s \
# RUN:   -o refold.obj
# RUN: lld-link -entry:main -subsystem:console refold.obj -out:refold.exe
# RUN: llvm-objdump -d --no-show-raw-insn refold.exe | \
# RUN:   FileCheck --check-prefix=REFOLD %s
# REFOLD:      140001000: adrp x0, 0x140202000
# REFOLD-NEXT: 140001004: add x0, x0, #0x4
# REFOLD-NEXT: 140001008: adr x1, 0x140101004
# REFOLD-NEXT: 14000100c: ret

## An adr is recorded as REL21 and a literal load as BRANCH19, whose field it
## has, each once.
# A64-FIXUP:      0005 | 0000  | 00001000 | 00003000
# A64-FIXUP-NEXT: 000F | 0000  | 00001004 | 00003000
# A64-FIXUP-NEXT: 000F | 0000  | 00001008 | 00003000
# A64-FIXUP-NEXT: 0004 | 0000  | 0000100C | 00003000

#--- x64.s
## a: a jcc over a jmp; jumps to the next section; a jump out of reach.
  .section .text$a,"xr",discard,a
  .globl a
a:
  jne .La1
  jmp .La2
.La1:
  nop
.La2:
  je g
  jmp g
  jmp far
## Not over this jmp, which leaves the section, nor over the next, which
## .Lmid names, nor over the last, which the jne does not jump over.
  jne .La3
  jmp g
.La3:
  jne .La4
.Lmid:
  jmp .La2
.La4:
  jne .La2
  jmp .La1
  retq
  movq __imp_g@FUSED(%rip), %rax
  .data
  .quad .Lmid

  .section .text$b,"xr",discard,g
  .globl g
g:
  retq

## c: the jne over the jmp becomes a je to .Lc, which the 32-bit jmp to far
## puts out of short reach; the jne's target begins with a nop, not the jmp.
  .section .text$c,"xr",discard,c
  .globl c
c:
  jne .Lc1
  jmp .Lc
.Lc1:
  nop
  jmp far
  .fill 124, 1, 0x90
.Lc:
  retq
  movq __imp_g@FUSED(%rip), %rax

## d: .Ldbg, which only debug information names, keeps no jmp.
  .section .text$d,"xr",discard,d
  .globl d
d:
  jne .Ld1
.Ldbg:
  jmp .Ld2
.Ld1:
  nop
.Ld2:
  retq
  movq __imp_g@FUSED(%rip), %rax
  .section .debug_line,"dr"
  .quad .Ldbg

  .section .text$z,"xr",discard,far
  .fill 200, 1, 0x90
  .globl far
far:
  retq

## The je is six bytes, padded off a 32-byte boundary, as assembled, and two
## bytes needing no padding in the image.
#--- bb.s
  .section .text$a,"xr",discard,bb
  .globl bb
  .p2align 5
bb:
  .fill 28, 1, 0x90
  je g
  retq
  movq __imp_g@FUSED(%rip), %rax

#--- a64.s
  .section .text$a,"xr",discard,a
  .globl a
a:
  adrp x0, cell
  add x0, x0, :lo12:cell
  adrp x1, cell
  ldr x1, [x1, :lo12:cell]
  adrp x2, cell
  ldrsw x2, [x2, :lo12:cell]
## Pairs that stay: another register, addends, two symbols, a load with no
## literal form, a symbol at the add, targets out of reach.
  adrp x3, cell
  add x4, x3, :lo12:cell
  adrp x5, cell+8
  add x5, x5, :lo12:cell
  adrp x5, cell
  add x5, x5, :lo12:cell+8
  adrp x5, cell
  ldr x5, [x5, :lo12:cell+8]
  adrp x5, cell
  add x5, x5, :lo12:cell2
  adrp x6, cell
  ldrb w6, [x6, :lo12:cell]
  adrp x7, cell
.Lmid:
  add x7, x7, :lo12:cell
  cbz x0, .Lmid
  adrp x8, far
  add x8, x8, :lo12:far
  adrp x9, far
  ldr x9, [x9, :lo12:far]
## A symbol at the add that only debug information names keeps no pair.
  adrp x10, cell
.Ldbg:
  add x10, x10, :lo12:cell
  adrp x11, cell+8
  add x11, x11, :lo12:cell+8
  adrp x12, cell2-8
  ldr x12, [x12, :lo12:cell2-8]
  adrp x13, far+8
  add x13, x13, :lo12:far+8
  ret
  .section .debug_line,"dr"
  .quad .Ldbg
  .data
cell:
  .quad 0
cell2:
  .quad 0
  .section .bss,"bw"
  .space 0x200000
far:
  .quad 0

#--- refold.s
  .text
  .globl main
main:
  adrp x0, far
  add x0, x0, :lo12:far
  adrp x1, edge
  add x1, x1, :lo12:edge
  ret
  .section .rdata,"dr"
  .space 0xff004
edge:
  .space 0x101000
far:
  .byte 0
