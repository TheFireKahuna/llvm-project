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

# CHECK:      IATRVA: 0x2110
# CHECK-NEXT: IATSize: 0x48

## The DLL's own descriptor, then its runs: the writable words, then the
## read-only chunks ro2 and ro3 as one run across them.
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x2090
# CHECK-NEXT: ImportAddressTableRVA: 0x2110
# CHECK-NEXT: Symbol: func1 (0)
# CHECK-NEXT: Symbol: var1 (1)
# CHECK-NEXT: Symbol: var2 (2)
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20C0
# CHECK-NEXT: ImportAddressTableRVA: 0x3000
# CHECK-NEXT: Symbol: func1 (0)
# CHECK-NEXT: Symbol: var1 (1)
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20D8
# CHECK-NEXT: ImportAddressTableRVA: 0x2140
# CHECK-NEXT: Symbol: var2 (2)
# CHECK-NEXT: Symbol: func1 (0)
## A local word ends a run.
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20B0
# CHECK-NEXT: ImportAddressTableRVA: 0x2130
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x20F0
# CHECK-NEXT: ImportAddressTableRVA: 0x3018
# CHECK-NEXT: Symbol: func2 (0)
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x2100
# CHECK-NEXT: ImportAddressTableRVA: 0x2150
# CHECK-NEXT: Symbol: func2 (0)

## Only the local word has a base relocation.
# CHECK:      BaseReloc [
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x3010
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: ABSOLUTE

## Each word holds its import's hint/name RVA: func1 0x2158, var1 0x2160,
## var2 0x2168, func2 0x2170.
# DATA:      140002140 68210000 00000000 58210000 00000000
# DATA-NEXT: 140002150 70210000 00000000
# DATA:      Contents of section .data:
# DATA-NEXT: 140003000 58210000 00000000 60210000 00000000
# DATA-NEXT: 140003010 00100040 01000000 70210000 00000000

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
