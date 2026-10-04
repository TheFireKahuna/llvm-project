# REQUIRES: aarch64

## On ARM64 under -import-slots, the adrp and ldr of a .refptr.X pointer become
## the adrp and add of X once X is defined in the image, a move of zero into
## each register once a weak X is absent, and the adrp and ldr of X's import
## pointer once X is imported. No pointer is left.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows defs.s -o defs.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows dll.s -o dll.obj
# RUN: lld-link -dll -noentry -machine:arm64 -out:dll.dll -implib:dll.lib \
# RUN:   dll.obj -export:g
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj defs.obj dll.lib 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s

# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: adrp x2, 0x[[#%x,IATPAGE:]]
# CHECK-NEXT: ldr x2, [x2, #0x[[#%x,IATOFF:]]]
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

# RELOC-NOT: Type: DIR64

#--- main.s
  .text
  .globl main
main:
  adrp x0, .refptr.f
  ldr x0, [x0, :lo12:.refptr.f]
  adrp x1, .refptr.absent
  ldr x1, [x1, :lo12:.refptr.absent]
  adrp x2, .refptr.g
  ldr x2, [x2, :lo12:.refptr.g]
  ret

  .weak f
  .weak absent
  .weak g

.macro refptr sym
  .section .rdata$.refptr.\sym,"dr",discard,.refptr.\sym
  .globl .refptr.\sym
  .p2align 3
.refptr.\sym:
  .xword \sym
.endm
  refptr f
  refptr absent
  refptr g

#--- defs.s
  .text
  .globl f
f:
  ret

#--- dll.s
  .text
  .globl g
g:
  ret
