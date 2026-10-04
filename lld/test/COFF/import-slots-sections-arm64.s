# REQUIRES: aarch64
## import-slots-sections.s on ARM64.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:arm64
# RUN: lld-link -machine:arm64 -import-slots -start-stop-symbols -opt:noref -entry:main \
# RUN:   -subsystem:console main.obj a.lib -debug:symtab -out:main.exe
# RUN: llvm-readobj --file-headers --sections --coff-imports \
# RUN:   --coff-basereloc main.exe | FileCheck %s
# RUN: llvm-nm main.exe | FileCheck --check-prefix=BOUNDS %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck --check-prefix=RDATA %s

# CHECK:      IATRVA: 0x2000
# CHECK-NEXT: IATSize: 0x2028
# CHECK:      Name: myro
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x2000
# CHECK:      Name: .CRT
# CHECK-NEXT: VirtualSize: 0x8
# CHECK-NEXT: VirtualAddress: 0x3000
# CHECK:      Name: .rdata
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x4000
# CHECK:      ImportAddressTableRVA: 0x4000
# CHECK:      ImportAddressTableRVA: 0x2000
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: var1 (1)
# CHECK:      ImportAddressTableRVA: 0x3000
# CHECK-NEXT: Symbol: f1 (0)
# CHECK:      ImportAddressTableRVA: 0x4020
# CHECK-NEXT: Symbol: var1 (1)
# CHECK:      BaseReloc [
# CHECK-NEXT: ]

# BOUNDS: 140002000 R __start_myro
# BOUNDS: 140002010 R __stop_myro

## The import address table, then .rdata$aa and .rdata$zz, then .rdata.
# RDATA:      140004000 e8400000 00000000 ee400000 00000000
# RDATA-NEXT: 140004010 00000000 00000000 00000000 00000000
# RDATA-NEXT: 140004020 ee400000 00000000 01000000 00000000

## A layout that puts a slot in an executable section is an error.
# RUN: not lld-link -machine:arm64 -import-slots -start-stop-symbols -opt:noref -entry:main \
# RUN:   -subsystem:console main.obj a.lib -merge:myro=.text -out:err.exe \
# RUN:   2>&1 | FileCheck --check-prefix=ERR %s
# ERR: error: main.obj: the address of an import at offset 0x0 in myro is in executable section .text

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  var1 DATA

#--- main.s
  .text
  .globl main
main:
  adrp x0, __start_myro
  add x0, x0, :lo12:__start_myro
  adrp x1, __stop_myro
  add x1, x1, :lo12:__stop_myro
  ret

  .section myro,"dr"
  .xword f1
  .xword var1
  .section .CRT$XCU,"dr"
  .xword f1
  .section .rdata$zz,"dr"
  .xword var1
  .section .rdata$aa,"dr"
  .xword 0
  .section .rdata,"dr"
  .xword 1
