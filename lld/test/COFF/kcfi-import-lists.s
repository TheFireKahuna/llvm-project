# REQUIRES: x86
## A KCFI type's list names an imported function by its import address table
## entry. Where static data holds the function's import thunk as its address,
## the linker adds to the type's list the address of a cell holding the thunk:
## when an object may take the function's address in an instruction it does
## not describe, without -import-slots, and for a delay-loaded function. With
## -import-slots, a function whose thunk is not its address gets no cell.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc list.s -o list.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc call.s -o call.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium load.s -o load.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium take.s -o take.obj

## call.obj does not describe its instructions, so f's thunk is its address.
## The cell, holding the thunk at 0x140001010, comes first; then the list's
## entries, __imp_f and the cell.
# RUN: lld-link -entry:main -subsystem:console -opt:noref -import-slots \
# RUN:   call.obj list.obj lib.lib -out:c6c.exe
# RUN: llvm-objdump -s -j .rdata c6c.exe | FileCheck --check-prefix=CELL %s
# RUN: lld-link -entry:main -subsystem:console -opt:noref call.obj list.obj \
# RUN:   lib.lib -out:noslots.exe
# RUN: llvm-objdump -s -j .rdata noslots.exe | FileCheck --check-prefix=CELL %s

# CELL:      140002000 10100040 01000000 50200040 01000000
# CELL-NEXT: 140002010 00200040 01000000

# RUN: lld-link -entry:main -subsystem:console -opt:noref -import-slots \
# RUN:   load.obj list.obj lib.lib -out:load.exe
# RUN: llvm-objdump -s -j .rdata load.exe | FileCheck --check-prefix=NOCELL %s

# NOCELL:      Contents of section .rdata:
# NOCELL-NEXT: 140002000 40200040 01000000 30200000 00000000

## The delay-loaded f's address-take becomes the address of its thunk.
# RUN: lld-link -entry:main -subsystem:console -opt:noref -import-slots \
# RUN:   take.obj list.obj lib.lib -delayload:lib.dll -out:delay.exe
# RUN: llvm-objdump -s -j .rdata delay.exe | FileCheck --check-prefix=DELAY %s

# DELAY:      140002000 10100040 01000000 00500040 01000000
# DELAY-NEXT: 140002010 00200040 01000000

#--- lib.def
LIBRARY lib.dll
EXPORTS
  f

#--- list.s
  .section .rdata$llvm_kcfi_11111111_m,"dr"
  .p2align 3
  .quad __imp_f
  .linkkcfilists

#--- call.s
  .text
  .globl main
main:
  callq f
  retq

#--- load.s
  .text
  .globl main
main:
  movq __imp_f(%rip), %rax
  retq

#--- take.s
  .text
  .globl main
main:
  leaq f(%rip), %rax
  retq
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq
