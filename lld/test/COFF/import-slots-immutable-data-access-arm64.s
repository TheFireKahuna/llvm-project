# REQUIRES: aarch64
## Under -import-slots, where an ldr of the import pointer of data that a DLL's
## instance preempts stays, since its register's address is stored, an access
## through the register that the object describes as one that only reads
## reads the image's own copy of the data from an offset from its own address
## where the copy is within reach, while the ldr and the store keep reaching
## the instance. A byte load, which has no such form, and an access out of
## reach read through the register.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium far.s -o far.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -out:main.exe -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d main.exe > main.txt
# RUN: llvm-objdump -s -j .rdata main.exe >> main.txt
# RUN: FileCheck --input-file=main.txt %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj far.obj \
# RUN:   lib.lib -out:far.exe
# RUN: llvm-objdump -d far.exe | FileCheck --check-prefix=FAR %s

# LOG: Keeping the copy of tbl in main.obj for 2 value reads

# CHECK:      <.text>:
# CHECK-NEXT: adrp x8,
# CHECK-NEXT: ldr x8, [x8,
# CHECK-NEXT: str x8, [x1]
# CHECK-NEXT: ldr w0, 0x[[#%x,TBL:]]
# CHECK-NEXT: ldrb w2, [x8, #0x1]
# CHECK-NEXT: ret
# CHECK:      Contents of section .rdata:
# CHECK:      [[#%x,TBL - 8]] 01000000 02000000 03000000 04000000

# FAR:      <.text>:
# FAR-NEXT: adrp x8,
# FAR-NEXT: ldr x8, [x8,
# FAR-NEXT: str x8, [x1]
# FAR-NEXT: ldr w0, [x8, #0x8]

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
  adrp x8, __imp_tbl
.Lr0:
  ldr x8, [x8, :lo12:__imp_tbl]
  str x8, [x1]
  .linkfusedaccess .Lr0, x8
  .linkvalueonly
  ldr w0, [x8, #8]
  .linkfusedaccess .Lr0, x8
  .linkvalueonly
  ldrb w2, [x8, #1]
  ret

#--- far.s
  .section .text$z,"xr"
  .space 0x100000
