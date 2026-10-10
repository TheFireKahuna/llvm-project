# REQUIRES: aarch64
## import-slots.s on ARM64: the same slots, runs, descriptors and layout.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:arm64
# RUN: lld-link -def:b.def -out:b.lib -machine:arm64
# RUN: lld-link -machine:arm64 -import-slots -opt:noref -entry:main -subsystem:console \
# RUN:   main.obj a.lib b.lib -out:main.exe
# RUN: llvm-readobj --file-headers --coff-imports --coff-basereloc main.exe | \
# RUN:   FileCheck %s
# RUN: llvm-objdump -s -j .rdata -j .data main.exe | \
# RUN:   FileCheck --check-prefix=DATA %s

## Without -import-slots, data offered only as __imp_var is not resolved.
# RUN: not lld-link -machine:arm64 -opt:noref -entry:main -subsystem:console main.obj a.lib \
# RUN:   b.lib -out:main.exe 2>&1 | FileCheck --check-prefix=NOSLOTS %s

# CHECK:      IATRVA: 0x20C8
# CHECK-NEXT: IATSize: 0x28

## The DLL's own descriptor holds var2, whose entry an instruction takes the
## address of; then its runs: the writable words, then the read-only chunks
## ro2 and ro3 as one run across them.
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x2078
# CHECK-NEXT: ImportAddressTableRVA: 0x20C8
# CHECK-NEXT: Symbol: var2 (2)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x2088
# CHECK-NEXT: ImportAddressTableRVA: 0x3000
# CHECK-NEXT: Symbol: func1 (0)
# CHECK-NEXT: Symbol: var1 (1)
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20A0
# CHECK-NEXT: ImportAddressTableRVA: 0x20D8
# CHECK-NEXT: Symbol: var2 (2)
# CHECK-NEXT: Symbol: func1 (0)
## func2 is used only through slots, so b.dll has no descriptor of its own. A
## local word ends a run, and the two runs share a lookup table.
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20B8
# CHECK-NEXT: ImportAddressTableRVA: 0x3018
# CHECK-NEXT: Symbol: func2 (0)
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20B8
# CHECK-NEXT: ImportAddressTableRVA: 0x20E8
# CHECK-NEXT: Symbol: func2 (0)
# CHECK-NOT:  Name:

## Only the local word has a base relocation.
# CHECK:      BaseReloc [
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x3010
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: ABSOLUTE

## Each word holds its import's hint/name RVA: func1 0x20F0, var1 0x20F8,
## var2 0x2100, func2 0x2108.
# DATA:      1400020d0 00000000 00000000 00210000 00000000
# DATA-NEXT: 1400020e0 f0200000 00000000 08210000 00000000
# DATA:      Contents of section .data:
# DATA-NEXT: 140003000 f0200000 00000000 f8200000 00000000
# DATA-NEXT: 140003010 00100040 01000000 08210000 00000000

# NOSLOTS: error: undefined symbol: var1

#--- a.def
LIBRARY a.dll
EXPORTS
  func1
  var1 DATA
  var2 DATA

#--- b.def
LIBRARY b.dll
EXPORTS
  func2

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_var2
  add x0, x0, :lo12:__imp_var2
  ret

  .data
  .globl wdata
wdata:
  .xword func1
  .xword var1
  .xword main
  .xword func2

  .section .rdata,"dr",one_only,ro1
  .globl ro1
ro1:
  .xword func2
  .section .rdata,"dr",one_only,ro2
  .globl ro2
ro2:
  .xword var2
  .section .rdata,"dr",one_only,ro3
  .globl ro3
ro3:
  .xword func1
