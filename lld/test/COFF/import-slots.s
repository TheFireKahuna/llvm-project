# REQUIRES: x86

# With -import-slots, static data that holds the address of an imported
# symbol is an in-place import slot: the loader writes the address there
# through an import descriptor whose address table is the slot itself. In the
# file the slot holds the import's lookup entry, and it has no base
# relocation. Slots of one DLL that are one word apart share a descriptor.
# Read-only chunks with slots are laid out after the import address table,
# grouped by DLL so that they form runs, and the IAT data directory covers
# them, since the loader makes only that range writable while it resolves
# imports. A slot that holds a function's address is listed in the Control
# Flow Guard address-taken IAT table. A function from a delay-loaded DLL keeps
# its thunk. A slot that holds an offset as well is import-slots-fill.s.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:variable,DATA -export:func -export:func2 -implib:%t.lib.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/other.s -o %t.other.obj
# RUN: lld-link -dll -noentry -out:%t.other.dll %t.other.obj -export:other,DATA -implib:%t.other.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/delay.s -o %t.delay.obj
# RUN: lld-link -dll -noentry -out:%t.delay.dll %t.delay.obj -export:delayfn -implib:%t.delay.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console -guard:cf -debug:symtab -out:%t.exe %t.main.obj %t.lib.lib %t.other.lib %t.delay.lib
# RUN: llvm-readobj --file-headers --coff-imports --coff-load-config --coff-basereloc %t.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata -j .data %t.exe | FileCheck --check-prefix=CONTENTS %s
# RUN: llvm-nm %t.exe | FileCheck --check-prefix=NM %s

# The address tables of the three DLLs (3 + 1, 1 + 1 and 1 + 1 words), then
# the read-only slot chunks: ro_run of lib.dll (2 + 1 words), then ro_other
# (1 word).
# CHECK:      IATRVA: 0x2270
# CHECK-NEXT: IATSize: 0x58

# CHECK:      Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x2270
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT:   Symbol: func2 (0)
# CHECK-NEXT:   Symbol: variable (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x3000
# CHECK-NEXT:   Symbol: func2 (0)
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x22B0
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT:   Symbol: variable (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.delay.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x2290
# CHECK-NEXT:   Symbol: delayfn (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.delay.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x3020
# CHECK-NEXT:   Symbol: delayfn (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.other.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x22A0
# CHECK-NEXT:   Symbol: other (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.other.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x3018
# CHECK-NEXT:   Symbol: other (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots.s.tmp.other.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x22C0
# CHECK-NEXT:   Symbol: other (0)
# CHECK-NEXT: }

# Base relocations: only the table pointer in the load configuration; none
# for a slot.
# CHECK:      BaseReloc [
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x20A0
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: ABSOLUTE
# CHECK-NEXT:     Address: 0x2000
# CHECK-NEXT:   }
# CHECK-NEXT: ]

# The four slots that hold a function's address.
# CHECK:      GuardAddressTakenIatEntryCount: 4
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x1400022B0
# CHECK-NEXT:   0x140003000
# CHECK-NEXT:   0x140003008
# CHECK-NEXT:   0x140003020
# CHECK-NEXT: ]

# Every slot holds its import's lookup entry, the RVA of the hint/name
# record: func 0x22C8, func2 0x22D0, variable 0x22D8, delayfn 0x22E4, other
# 0x22EE.
# CONTENTS:      Contents of section .rdata:
# CONTENTS:      1400022a0 ee220000 00000000 00000000 00000000
# CONTENTS-NEXT: 1400022b0 c8220000 00000000 d8220000 00000000
# CONTENTS-NEXT: 1400022c0 ee220000 00000000
# CONTENTS:      Contents of section .data:
# CONTENTS-NEXT: 140003000 d0220000 00000000 c8220000 00000000
# CONTENTS-NEXT: 140003010 00000000 00000000 ee220000 00000000
# CONTENTS-NEXT: 140003020 e4220000 00000000

# NM-DAG: 1400022b0 R ro_run
# NM-DAG: 1400022c0 R ro_other

# RUN: lld-link -import-slots -entry:main -subsystem:console -debug:symtab -out:%t.delayed.exe %t.main.obj %t.lib.lib %t.other.lib %t.delay.lib -delayload:%basename_t.tmp.delay.dll -alternatename:__delayLoadHelper2=main
# RUN: llvm-readobj --coff-imports --coff-basereloc %t.delayed.exe | FileCheck --check-prefix=DELAY %s

# rw_delay holds the thunk's address, with a base relocation, and delay.dll
# has no in-place descriptor.
# DELAY-NOT:  Name: import-slots.s.tmp.delay.dll
# DELAY:      DelayImport {
# DELAY-NEXT:   Name: import-slots.s.tmp.delay.dll
# DELAY:      BaseReloc [
# DELAY-NEXT:   Entry {
# DELAY-NEXT:     Type: DIR64
# DELAY-NEXT:     Address: 0x3020

#--- lib.s
.text
.globl func
func:
  ret
.globl func2
func2:
  ret
.data
.globl variable
variable:
  .quad 1
  .quad 2

#--- other.s
.data
.globl other
other:
  .quad 3

#--- delay.s
.text
.globl delayfn
delayfn:
  ret

#--- main.s
.def @feat.00
.scl 3
.type 0
.endef
.globl @feat.00
@feat.00 = 0x800

.text
.globl main
main:
  ret

.section .rdata,"dr"
.p2align 3
.globl _load_config_used
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .fill 84, 1, 0

.section .gfids$y,"dr"
.section .giats$y,"dr"
.section .gljmp$y,"dr"

# Read-only slots, each group its own chunk. ro_other separates the two
# lib.dll chunks in the object; the layout puts them next to each other.
.section .rdata,"dr",one_only,ro_run
.p2align 3
.globl ro_run
ro_run:
  .quad func
  .quad variable

.section .rdata,"dr",one_only,ro_other
.p2align 3
.globl ro_other
ro_other:
  .quad other

# Writable slots: a run of two, a plain word that ends it, and one of each
# other DLL.
.data
.globl rw_run
rw_run:
  .quad func2
  .quad func
  .quad 0
.globl rw_other
rw_other:
  .quad other
.globl rw_delay
rw_delay:
  .quad delayfn

