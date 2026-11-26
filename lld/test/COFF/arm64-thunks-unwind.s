# REQUIRES: aarch64

## A range thunk that a branch inside an unwind frame reaches has a function
## table entry whose record runs that frame's codes at each of its words,
## after an end_c: ntdll would otherwise unwind it as a leaf, with the frame
## open. A thunk that a call or a branch outside any frame reaches has none.

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console %t.obj -out:%t.exe
# RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck %s
# RUN: llvm-readobj --unwind %t.exe | FileCheck --check-prefix=UNWIND %s

# CHECK:      140001004: tbz w0, #0x0, 0x14000101c
# CHECK:      140001014: tbz w0, #0x0, 0x140001028
# CHECK:      14000101c: adrp x16
# CHECK:      140001028: adrp x16

# UNWIND:      Function: 0x140001000
# UNWIND:      Function: 0x14000101C
# UNWIND:        FunctionLength: 12
# UNWIND:        Prologue [
# UNWIND-NEXT:     0xe5 ; end_c
# UNWIND-NEXT:     0x81 ; stp x29, x30, [sp, #-16]!
# UNWIND-NEXT:     0xe4 ; end
# UNWIND-NOT:  Function:

  .globl main
  .seh_proc main
main:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  .seh_endprologue
  tbz w0, #0, cold
  bl leaf
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
  .seh_endproc
leaf:
  tbz w0, #0, cold
  ret
  .section .text$b,"xr"
  .space 0x8000
cold:
  ret
