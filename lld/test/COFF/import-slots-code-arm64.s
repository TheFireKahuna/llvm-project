# REQUIRES: aarch64
## import-slots-code.s on ARM64, where sites are classified by relocation type
## in any object: an adrp and add pair passing lld/ELF's pairing check becomes
## adrp and ldr of the import address table entry (C6b); a pair that fails it,
## or an adr, makes the image use the thunk as the function's address (C6c).
## Every adrp and ldr of a delay-loaded function's entry becomes adrp and add
## of its thunk, which static data holds. Branches are unchanged.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:arm64
# RUN: lld-link -def:b.def -out:b.lib -machine:arm64
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   main.obj a.lib b.lib -delayload:b.dll -out:code.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d code.exe | FileCheck %s
# RUN: llvm-objdump -s -j .data code.exe | FileCheck --check-prefix=DATA %s

# WARN: warning: main.obj: may take the address of f2, imported from a.dll, in an instruction it does not describe, so the image uses its import thunk as its address; declare it imported, or rebuild the object with clang
# WARN: warning: main.obj: may take the address of f3, imported from a.dll, in an instruction it does not describe, so the image uses its import thunk as its address; declare it imported, or rebuild the object with clang

# CHECK:      <.text>:
# CHECK-NEXT: adrp x0, 0x140002000
# CHECK-NEXT: ldr x0, [x0, #0xc8]
# CHECK-NEXT: bl 0x14000102c
# CHECK-NEXT: adrp x1, 0x140001000
# CHECK-NEXT: add x2, x1, #0x38
# CHECK-NEXT: adr x3, 0x140001044
# CHECK-NEXT: adrp x4, 0x140001000
# CHECK-NEXT: add x4, x4, #0x50
# CHECK-NEXT: blr x4

## f1 is in place; f2, f3 and g hold their thunks.
# DATA: 140003000 e8200000 00000000 38100040 01000000
# DATA: 140003010 44100040 01000000 50100040 01000000

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  f2
  f3

#--- b.def
LIBRARY b.dll
EXPORTS
  g

#--- main.s
  .text
  .globl main
main:
  adrp x0, f1
  add x0, x0, :lo12:f1
  bl f1
  adrp x1, f2
  add x2, x1, :lo12:f2
  adr x3, f3
  adrp x4, __imp_g
  ldr x4, [x4, :lo12:__imp_g]
  blr x4
  ret
  .globl __delayLoadHelper2
__delayLoadHelper2:
  ret

  .data
  .xword f1
  .xword f2
  .xword f3
  .xword g
