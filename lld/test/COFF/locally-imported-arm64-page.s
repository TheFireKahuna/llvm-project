# REQUIRES: aarch64

## On ARM64, when every reference to a local import pointer is the adrp or the
## 64-bit ldr of a load of it, each becomes the adrp and add of its symbol, and
## the pointer is left out of the image. One other reference to the pointer
## keeps every load of it, since an adrp may serve several loads.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d --no-show-raw-insn a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s

# WARN-NOT: locally defined symbol imported: f
# WARN:     warning: main.obj: locally defined symbol imported: g (defined in defs.obj) [LNK4217]
# WARN-NOT: locally defined symbol imported: f

# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: adrp x1, 0x[[#%x,PTRPAGE:]]
# CHECK-NEXT: ldr x1, [x1]
# CHECK-NEXT: adrp x2, 0x[[#PTRPAGE]]
# CHECK-NEXT: add x2, x2, #0x0
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

## The pointer to g remains; there is none to f.
# RELOC-COUNT-1: Type: DIR64
# RELOC-NOT:     Type: DIR64

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  adrp x1, __imp_g
  ldr x1, [x1, :lo12:__imp_g]
  adrp x2, __imp_g
  add x2, x2, :lo12:__imp_g
  ret

#--- defs.s
  .text
  .globl f, g
  .p2align 2
g:
  ret
f:
  ret
