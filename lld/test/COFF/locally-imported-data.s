# REQUIRES: x86

## Under -import-slots, a reference from data to an import pointer, such as a
## catch-type entry, is a pointer the compiler made: it reads the pointer of a
## symbol in the image, keeps no instruction from being rewritten, and is not
## reported as a locally defined symbol imported. A reference to an imported
## symbol reads its import address table entry.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: lld-link -dll -noentry -export:imp -out:lib.dll -implib:lib.lib lib.obj

# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   -map:a.map main.obj defs.obj lib.lib 2>&1 | count 0
# RUN: FileCheck --check-prefix=MAP %s < a.map
# RUN: llvm-objdump -d -s a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s

# MAP: 0002:0000005e 00000008H .xdata
# MAP: 0002:00000000 __imp_f
# MAP: 0002:00000040 __imp_imp {{.*}} lib:lib.dll

## The entries at 0x205e and 0x2062 hold -0x5e and -0x22: the pointer to f
## and the import address table entry of imp. One local pointer only, since
## the order of several is not fixed.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: 140002000 08100040 01000000
# CHECK:      140002050 0000696d 70006c69 622e646c 6c00a2ff
# CHECK-NEXT: 140002060 ffffdeff ffff

## The call through the pointer to f still reaches f directly.
# CHECK: 67 e8 02 00 00 00 addr32 callq 0x140001008

# RELOC-COUNT-1: Type: DIR64
# RELOC-NOT:     Type: DIR64

## Without -import-slots each reference from data is reported, as link.exe
## reports it.
# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj defs.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=WARN %s

# WARN: warning: main.obj: locally defined symbol imported: f (defined in defs.obj) [LNK4217]

#--- main.s
  .text
  .globl main
main:
  callq *__imp_f(%rip)
  retq

  .section .xdata,"dr"
  .long __imp_f-.
  .long __imp_imp-.

#--- defs.s
  .text
  .globl f
f:
  retq

#--- lib.s
  .text
  .globl imp
imp:
  retq
