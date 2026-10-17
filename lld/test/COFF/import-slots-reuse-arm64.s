# REQUIRES: aarch64
## import-slots-reuse.s on ARM64: code that loads an import's whole address
## with adrp and ldr, the import thunk and the load that an address-take
## becomes read the slot the import is kept in, every such instruction moving
## with it. An add that takes the entry's address and a 32-bit load each keep
## the entry.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:arm64
# RUN: lld-link -machine:arm64 -import-slots -opt:noref -entry:main \
# RUN:   -subsystem:console main.obj a.lib -debug:symtab -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | \
# RUN:   FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-nm -n main.exe > out.txt
# RUN: llvm-objdump -d --no-show-raw-insn main.exe >> out.txt
# RUN: FileCheck %s < out.txt

# IMPORTS:      Name: a.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA:
# IMPORTS-NEXT: Symbol: lea (1)
# IMPORTS-NEXT: Symbol: narrow (3)
# IMPORTS-NEXT: }

# CHECK:      [[#%x,RO:]] R __imp_load
# CHECK-NEXT: [[#RO + 8]] R __imp_thunk
# CHECK-NEXT: [[#RO + 16]] R __imp_addr

# CHECK:      <main>:
# CHECK-NEXT: adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: ldr x0, [x0, #0x[[#%x,RO - PAGE]]]
# CHECK-NEXT: adrp x1,
# CHECK-NEXT: add x1, x1,
# CHECK-NEXT: adrp x2,
# CHECK-NEXT: ldr w2, [x2,
# CHECK-NEXT: bl 0x[[#%x,THUNK:]]
# CHECK-NEXT: adrp x3, 0x[[#PAGE]]
# CHECK-NEXT: ldr x3, [x3, #0x[[#%x,RO + 16 - PAGE]]]
# CHECK:      [[#THUNK]]: adrp x16, 0x[[#PAGE]]
# CHECK-NEXT: ldr x16, [x16, #0x[[#%x,RO + 8 - PAGE]]]
# CHECK-NEXT: br x16

#--- a.def
LIBRARY a.dll
EXPORTS
  addr
  lea
  load
  narrow
  thunk

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_load
  ldr x0, [x0, :lo12:__imp_load]
  adrp x1, __imp_lea
  add x1, x1, :lo12:__imp_lea
  adrp x2, __imp_narrow
  ldr w2, [x2, :lo12:__imp_narrow]
  bl thunk
  adrp x3, addr
  add x3, x3, :lo12:addr
  ret

  .section .rdata,"dr"
  .p2align 3
  .xword load
  .xword thunk
  .xword addr
  .xword lea
  .xword narrow
