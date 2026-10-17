# REQUIRES: aarch64
## import-slots-weak-data.s on ARM64: an adrp and add pair that takes the
## address of preempted data and passes lld/ELF's pairing check becomes adrp
## and ldr of the import address table entry, the group's sections are
## discarded with the initializer, and a word of static data becomes an
## in-place slot.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   main.obj lib.lib -out:out.exe
# RUN: llvm-objdump -d out.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports out.exe | FileCheck --check-prefix=IMPORTS %s

# CHECK:      <.text>:
# CHECK-NEXT: adrp x0, 0x140002000
# CHECK-NEXT: ldr x0, [x0, #0x[[#%x,V:]]]
# CHECK-NEXT: adrp x1, 0x140002000
# CHECK-NEXT: ldr x1, [x1, #0x[[#V]]]
# CHECK-NEXT: ret
# CHECK-NOT:  mov w2, #0x5678

## The guard's entry, which only the discarded initializer used, is left out.
# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA:
# IMPORTS-NEXT: Symbol: _ZN1A1vE (1)
# IMPORTS-NEXT: }
# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA: 0x3000
# IMPORTS-NEXT: Symbol: _ZN1A1vE (1)
# IMPORTS-NEXT: }

#--- lib.def
LIBRARY lib.dll
EXPORTS
  _ZN1A1vE DATA
  _ZGVN1A1vE DATA

#--- main.s
  .section .bss,"bw",discard,_ZN1A1vE
  .globl _ZN1A1vE
  .p2align 3
_ZN1A1vE:
  .zero 8

  .section .bss,"bw",discard,_ZGVN1A1vE
  .globl _ZGVN1A1vE
_ZGVN1A1vE:
  .xword 0

  .section .text,"xr",associative,_ZN1A1vE
init:
  adrp x1, __imp__ZGVN1A1vE
  ldr x1, [x1, :lo12:__imp__ZGVN1A1vE]
  mov w2, #0x5678
  ret

  .section .CRT$XCU,"dr",associative,_ZN1A1vE
  .xword init

  .text
  .globl main
main:
  adrp x0, __imp__ZN1A1vE
  ldr x0, [x0, :lo12:__imp__ZN1A1vE]
  adrp x1, _ZN1A1vE
  add x1, x1, :lo12:_ZN1A1vE
  ret

  .data
  .xword _ZN1A1vE
