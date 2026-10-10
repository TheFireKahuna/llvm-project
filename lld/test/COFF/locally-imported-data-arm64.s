# REQUIRES: aarch64

## On ARM64 under -import-slots, a reference from data to the import pointer of
## a symbol in the image keeps the pointer, but not its adrp and ldr loads,
## which still become the adrp and add of the symbol. It is not reported as a
## locally defined symbol imported.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium defs.s -o defs.obj

# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   -map:a.map main.obj defs.obj 2>&1 | count 0
# RUN: FileCheck --check-prefix=MAP %s < a.map
# RUN: llvm-objdump -d -s --no-show-raw-insn a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s

# MAP: 0002:00000000 __imp_f

## The pointer holds f, and the entry at 0x2008 holds -8, reaching it.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: 140002000 0c100040 01000000 f8ffffff

# CHECK:      adrp x0, 0x140001000
# CHECK-NEXT: add x0, x0, #0xc
# CHECK-NEXT: ret
# CHECK-NEXT: 14000100c: ret

# RELOC-COUNT-1: Type: DIR64
# RELOC-NOT:     Type: DIR64

## Without -import-slots the reference from data keeps the loads, as before,
## and is reported.
# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj defs.obj \
# RUN:   2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d --no-show-raw-insn b.exe | FileCheck --check-prefix=KEEP %s

# WARN: warning: main.obj: locally defined symbol imported: f (defined in defs.obj) [LNK4217]

# KEEP:      adrp x0, 0x140002000
# KEEP-NEXT: ldr x0, [x0]

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  ret

  .section .xdata,"dr"
  .word __imp_f-.

#--- defs.s
  .text
  .globl f
  .p2align 2
f:
  ret
