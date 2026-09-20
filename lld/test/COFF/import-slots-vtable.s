# REQUIRES: x86

# A vtable entry that holds the address of an imported function is not an
# in-place import slot: it holds the import thunk's address, with a base
# relocation, as it does on MSVC. A vtable's layout is fixed, so a slot there
# would need an import descriptor of its own and would move the vtable into
# the import address table region. The entry is only ever called through, so
# the thunk is not observable; the thunk is what the Control Flow Guard
# address-taken table lists. Data outside a vtable keeps its slot.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:func -implib:%t.lib.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console -guard:cf -debug:symtab -out:%t.exe %t.main.obj %t.lib.lib
# RUN: llvm-readobj --coff-imports --coff-basereloc --coff-load-config %t.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata %t.exe | FileCheck --check-prefix=CONTENTS %s
# RUN: llvm-nm %t.exe | FileCheck --check-prefix=NM %s

# One descriptor for the DLL itself, one for the slot in plain read-only
# data. The two vtables add none.
# CHECK:      Import {
# CHECK-NEXT:   Name: import-slots-vtable.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x2170
# CHECK-NEXT:   ImportAddressTableRVA: 0x2190
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots-vtable.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x2180
# CHECK-NEXT:   ImportAddressTableRVA: 0x21A0
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }
# CHECK-NOT:  Name: import-slots-vtable.s.tmp.lib.dll

# The vtable entries are relocated; the slot at 0x21A0 is not.
# CHECK:      BaseReloc [
# CHECK:        Address: 0x2110
# CHECK:        Address: 0x2120
# CHECK-NOT:    Address: 0x21A0

# The vtables hand out the thunk, so the thunk is the address-taken
# function; only the slot is an address-taken import entry.
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x140001010
# CHECK-NEXT: ]
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x1400021A0
# CHECK-NEXT: ]

# Both vtable entries hold the thunk's address, 0x140001010. The slot holds
# func's lookup entry, the RVA of its hint/name record.
# CONTENTS:      Contents of section .rdata:
# CONTENTS:      140002110 10100040 01000000
# CONTENTS-NEXT: 140002120 10100040 01000000
# CONTENTS:      1400021a0 a8210000 00000000

# NM-DAG: 140002108 R _ZTV3Foo
# NM-DAG: 140002118 R _ZTC3Bar
# NM-DAG: 1400021a0 R ro_slot

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
