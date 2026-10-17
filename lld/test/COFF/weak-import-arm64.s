# REQUIRES: aarch64

## On ARM64, the adrp and ldr of __imp_X for an extern_weak X, which the object
## keeps a weak external, become the adrp and add of X once X is defined in
## the image, a move of zero once X is absent, and stay the adrp and ldr of X's
## import address table entry once an import library offers X. No local
## import pointer is left, and none is reported.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium defs.s -o defs.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium dll.s -o dll.obj
# RUN: lld-link -dll -noentry -machine:arm64 -out:dll.dll -implib:dll.lib \
# RUN:   dll.obj -export:g
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj defs.obj dll.lib 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn a.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports --coff-basereloc a.exe | \
# RUN:   FileCheck --check-prefix=IMAGE %s

# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: adrp x2, 0x140002000
# CHECK-NEXT: ldr x2, [x2, #0x38]
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

# IMAGE:      Name: dll.dll
# IMAGE:      Symbol: g
# IMAGE:      BaseReloc [
# IMAGE-NEXT: ]

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  adrp x1, __imp_absent
  ldr x1, [x1, :lo12:__imp_absent]
  adrp x2, __imp_g
  ldr x2, [x2, :lo12:__imp_g]
  ret

  .weak f
  .weak absent
  .weak g

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
