# REQUIRES: x86
## A read-only section other than .rdata that holds an in-place import slot
## keeps its name and its place in its program's order: it is laid out before
## .rdata, whose start holds the import address tables and, when one of them
## holds a slot, the $-groups of .rdata in their order, and the import address
## table directory covers them all. A section bounded by __start_ and __stop_
## symbols stays bounded.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -import-slots -start-stop-symbols -opt:noref -entry:main \
# RUN:   -subsystem:console main.obj a.lib -debug:symtab -out:main.exe
# RUN: llvm-readobj --file-headers --sections --coff-imports \
# RUN:   --coff-basereloc main.exe | FileCheck %s
# RUN: llvm-nm main.exe | FileCheck --check-prefix=BOUNDS %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck --check-prefix=RDATA %s

# CHECK:      IATRVA: 0x2000
# CHECK-NEXT: IATSize: 0x2010
# CHECK:      Name: myro
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x2000
# CHECK:      Name: .CRT
# CHECK-NEXT: VirtualSize: 0x8
# CHECK-NEXT: VirtualAddress: 0x3000
# CHECK:      Name: .rdata
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x4000
# CHECK:      ImportAddressTableRVA: 0x2000
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: var1 (1)
# CHECK:      ImportAddressTableRVA: 0x3000
# CHECK-NEXT: Symbol: f1 (0)
# CHECK:      ImportAddressTableRVA: 0x4008
# CHECK-NEXT: Symbol: var1 (1)
# CHECK:      BaseReloc [
# CHECK-NEXT: ]

# BOUNDS: 140002000 R __start_myro
# BOUNDS: 140002010 R __stop_myro

## Both imports are kept in slots alone, so .rdata starts with no import
## address table: .rdata$aa and .rdata$zz, then .rdata.
# RDATA:      140004000 00000000 00000000 96400000 00000000
# RDATA-NEXT: 140004010 01000000 00000000

## A layout that puts a slot in an executable section is an error.
# RUN: not lld-link -import-slots -start-stop-symbols -opt:noref -entry:main \
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
  leaq __start_myro(%rip), %rax
  leaq __stop_myro(%rip), %rax
  retq

  .section myro,"dr"
  .quad f1
  .quad var1
  .section .CRT$XCU,"dr"
  .quad f1
  .section .rdata$zz,"dr"
  .quad var1
  .section .rdata$aa,"dr"
  .quad 0
  .section .rdata,"dr"
  .quad 1
