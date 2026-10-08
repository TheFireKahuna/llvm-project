# REQUIRES: aarch64

## On ARM64, with no -import-slots and in MinGW mode, the adrp and ldr of a
## .refptr.X pointer become the adrp and add of X once X is defined in the
## image, and the pointer is left out, unless something else reads it, which
## keeps it where it is. Collecting sections or not, as MinGW links by default,
## makes no difference.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-gnu main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-gnu defs.s -o defs.obj
# RUN: lld-link -lldmingw -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj defs.obj 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s
# RUN: llvm-objdump -s -j .rdata a.exe | FileCheck --check-prefix=RDATA %s
# RUN: lld-link -lldmingw -opt:noref -entry:main -subsystem:console \
# RUN:   -out:b.exe main.obj defs.obj 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn b.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc b.exe | FileCheck --check-prefix=RELOC %s
# RUN: llvm-objdump -s -j .rdata b.exe | FileCheck --check-prefix=RDATA %s

# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: adrp x1, 0x140002000
# CHECK-NEXT: ldr x1, [x1]
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

## The pointer to g, which data reads, and the word of data that holds its
## address.
# RELOC-COUNT-2: Type: DIR64
# RELOC-NOT:     Type: DIR64

# RDATA:      Contents of section .rdata:
# RDATA-NEXT: 140002000 {{[0-9a-f]+}} 01000000

#--- main.s
  .text
  .globl main
main:
  adrp x0, .refptr.f
  ldr x0, [x0, :lo12:.refptr.f]
  adrp x1, .refptr.g
  ldr x1, [x1, :lo12:.refptr.g]
  ret

  .data
  .p2align 3
  .xword .refptr.g

.macro refptr sym
  .section .rdata$.refptr.\sym,"dr",discard,.refptr.\sym
  .globl .refptr.\sym
  .p2align 3
.refptr.\sym:
  .xword \sym
.endm
  refptr f
  refptr g

#--- defs.s
  .text
  .globl f, g
f:
  ret
g:
  ret
