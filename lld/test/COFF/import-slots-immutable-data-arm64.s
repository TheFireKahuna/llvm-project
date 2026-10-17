# REQUIRES: aarch64
## Under -import-slots, an ldr of the import pointer of data that a DLL's
## instance preempts, which the object describes as one whose register only
## reads of memory read, becomes an add with its adrp, computing the address
## of the image's own copy, kept with no symbol, while every other reference
## reaches the instance. An access that the object describes through such a
## load takes the copy's offset in its page, the ldr becoming a nop.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -out:main.exe -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d main.exe > main.txt
# RUN: llvm-objdump -s -j .rdata main.exe >> main.txt
# RUN: FileCheck --input-file=main.txt %s
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=IMPORTS %s

# LOG: Keeping the copy of tbl in main.obj for 2 value reads

# CHECK:      <.text>:
# CHECK-NEXT: adrp x8, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x8, x8, #0x[[#%x,OFF:]]
# CHECK-NEXT: ldr w0, [x8, x0, lsl #2]
# CHECK-NEXT: adrp x9,
# CHECK-NEXT: ldr x9, [x9,
# CHECK-NEXT: ret
# CHECK-NEXT: adrp x8, 0x[[#PAGE]]
# CHECK-NEXT: nop
# CHECK-NEXT: ldr w0, [x8, #0x[[#OFF+8]]]
# CHECK-NEXT: ret
# CHECK:      Contents of section .rdata:
# CHECK:      [[#%x,PAGE+OFF]] 01000000 02000000 03000000 04000000

# IMPORTS:      Name: lib.dll
# IMPORTS:      Symbol: tbl
# IMPORTS-NEXT: }

## Range extension thunks leave the load reading the copy.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium thunk.s -o thunk.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console thunk.obj lib.lib \
# RUN:   -out:thunk.exe -verbose 2>&1 | FileCheck --check-prefix=THUNKLOG %s
# RUN: llvm-objdump -d thunk.exe | FileCheck --check-prefix=THUNK %s

# THUNKLOG: Keeping the copy of tbl in thunk.obj for 1 value read
# THUNKLOG: Added 1 thunks with margin {{.*}} in 1 passes

# THUNK:      <.text>:
# THUNK-NEXT: tbz w0, #0x0,
# THUNK-NEXT: adrp x8,
# THUNK-NEXT: add x8, x8,
# THUNK-NEXT: ldr w0, [x8, x1, lsl #2]
# THUNK-NEXT: ret

#--- lib.def
LIBRARY lib.dll
EXPORTS
  tbl DATA

#--- main.s
  .section .rdata,"dr",discard,tbl
  .globl tbl
  .p2align 4
tbl:
  .word 1, 2, 3, 4

  .text
  .globl main
main:
.Lp0:
  adrp x8, __imp_tbl
  .linkvalueonly .Lp0
  ldr x8, [x8, :lo12:__imp_tbl]
  ldr w0, [x8, x0, lsl #2]
  adrp x9, __imp_tbl
  ldr x9, [x9, :lo12:__imp_tbl]
  ret

  .globl g
g:
.Lp1:
  adrp x8, __imp_tbl
  .linkvalueonly .Lp1
.Lr0:
  .linkfusedread
  ldr x8, [x8, :lo12:__imp_tbl]
  .linkfusedaccess .Lr0, x8
  ldr w0, [x8, #8]
  ret

#--- thunk.s
  .section .rdata,"dr",discard,tbl
  .globl tbl
  .p2align 4
tbl:
  .word 1, 2, 3, 4

  .text
  .globl main
main:
  tbz w0, #0, far
.Lp0:
  adrp x8, __imp_tbl
  .linkvalueonly .Lp0
  ldr x8, [x8, :lo12:__imp_tbl]
  ldr w0, [x8, x1, lsl #2]
  ret

  .section .text$a,"xr"
  .space 0x8000
  .section .text$b,"xr"
far:
  ret
