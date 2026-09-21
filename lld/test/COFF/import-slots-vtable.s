# REQUIRES: x86

# A vtable entry that holds the address of an imported function is an
# in-place import slot, like any other static data: the loader writes the
# function's own address there, so the entry has no base relocation and the
# vtable is laid out with the import address table. An entry between local
# ones is a run of one, with a descriptor of its own. The entry, not a thunk,
# is what the Control Flow Guard address-taken IAT table lists; the thunk that
# nothing reaches any more is left out of the image. A function from a
# delay-loaded DLL keeps its thunk, with a base relocation.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:func -implib:%t.lib.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/delay.s -o %t.delay.obj
# RUN: lld-link -dll -noentry -out:%t.delay.dll %t.delay.obj -export:delayfn -implib:%t.delay.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/delayvt.s -o %t.delayvt.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console -guard:cf -debug:symtab -out:%t.exe %t.main.obj %t.lib.lib
# RUN: llvm-readobj --file-headers --coff-imports --coff-basereloc --coff-load-config %t.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck --check-prefix=CONTENTS %s
# RUN: llvm-nm %t.exe | FileCheck --check-prefix=NM %s

# The import address table directory covers both vtables and the slot.
# CHECK:      IATRVA: 0x21A0
# CHECK-NEXT: IATSize: 0x38

# The DLL's own descriptor, then one for each vtable. The entry of the
# construction vtable is the last word of its chunk, and the slot's chunk
# follows it directly, so the two share a descriptor.
# CHECK:      Import {
# CHECK-NEXT:   Name: import-slots-vtable.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x2168
# CHECK-NEXT:   ImportAddressTableRVA: 0x21A0
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots-vtable.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x2178
# CHECK-NEXT:   ImportAddressTableRVA: 0x21B8
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots-vtable.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x2188
# CHECK-NEXT:   ImportAddressTableRVA: 0x21C8
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }

# Only the load configuration's pointer to the IAT table is relocated; the
# second entry is padding.
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

# No thunk is left to list as address-taken; the three entries are.
# CHECK:      GuardCFFunctionCount: 0
# CHECK:      GuardAddressTakenIatEntryCount: 3
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x1400021B8
# CHECK-NEXT:   0x1400021C8
# CHECK-NEXT:   0x1400021D0
# CHECK-NEXT: ]

# Every entry holds func's lookup entry, the RVA of its hint/name record.
# CONTENTS:      Contents of section .rdata:
# CONTENTS:      1400021b0 00000000 00000000 d8210000 00000000
# CONTENTS-NEXT: 1400021c0 00000000 00000000 d8210000 00000000
# CONTENTS-NEXT: 1400021d0 d8210000 00000000

# The thunk is gone, so main is all of .text.
# NM-NOT: {{ }}func
# NM-DAG: 1400021b0 R _ZTV3Foo
# NM-DAG: 1400021c0 R _ZTC3Bar
# NM-DAG: 1400021d0 R ro_slot
# NM-DAG: 140001000 T main

# RUN: lld-link -import-slots -entry:main -subsystem:console -debug:symtab -out:%t.delayed.exe %t.main.obj %t.delayvt.obj %t.lib.lib %t.delay.lib -delayload:%basename_t.tmp.delay.dll -alternatename:__delayLoadHelper2=main
# RUN: llvm-readobj --coff-imports --coff-basereloc %t.delayed.exe | FileCheck --check-prefix=DELAY %s
# RUN: llvm-nm %t.delayed.exe | FileCheck --check-prefix=DELAYNM %s
# RUN: llvm-objdump -s -j .rdata %t.delayed.exe | FileCheck --check-prefix=DELAYCONTENTS %s

# The entry of _ZTV3Baz, at 0x2110, holds delayfn's thunk and is relocated;
# the vtable stays with the rest of the read-only data.
# DELAY:      DelayImport {
# DELAY-NEXT:   Name: import-slots-vtable.s.tmp.delay.dll
# DELAY:      BaseReloc [
# DELAY:        Address: 0x2110
# DELAYNM-DAG: 140001010 T delayfn
# DELAYNM-DAG: 140002108 R _ZTV3Baz
# DELAYCONTENTS: 140002110 10100040 01000000
#--- lib.s
.text
.globl func
func:
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

# A vtable and a construction vtable, each with an imported entry.
.section .rdata,"dr",one_only,_ZTV3Foo
.p2align 3
.globl _ZTV3Foo
_ZTV3Foo:
  .quad 0
  .quad func

.section .rdata,"dr",one_only,_ZTC3Bar
.p2align 3
.globl _ZTC3Bar
_ZTC3Bar:
  .quad 0
  .quad func

.section .rdata,"dr",one_only,ro_slot
.p2align 3
.globl ro_slot
ro_slot:
  .quad func

# The compiler names the function whose address a vtable holds; the linker
# resolves it to the thunk.
.section .gfids$y,"dr"
.symidx func
.section .giats$y,"dr"
.section .gljmp$y,"dr"

#--- delay.s
.text
.globl delayfn
delayfn:
  ret

#--- delayvt.s
# A vtable whose entry names a function of the delay-loaded DLL.
.section .rdata,"dr",one_only,_ZTV3Baz
.p2align 3
.globl _ZTV3Baz
_ZTV3Baz:
  .quad 0
  .quad delayfn
