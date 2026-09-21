# REQUIRES: x86

# An in-place import slot whose word holds an offset as well as an imported
# address cannot be written by the loader, which writes exact exports only.
# The linker writes a function that loads the import's address from the
# import address table, adds the offset and stores the sum, and calls it as
# the first C initializer. The slot lives in writable data, as the pointer
# MSVC's compiler initializes at startup does: a read-only chunk holding one
# moves to .data, where its plain slots stay the loader's. The slot has no
# base relocation and holds zero until the function runs; the function is
# listed as a Control Flow Guard target, since the C runtime calls it through
# its table.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:variable,DATA -export:func -implib:%t.lib.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console -guard:cf -debug:symtab -out:%t.exe %t.main.obj %t.lib.lib
# RUN: llvm-readobj --coff-imports --coff-basereloc --coff-load-config %t.exe | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck --check-prefix=CODE %s
# RUN: llvm-objdump -s -j .data -j .CRT %t.exe | FileCheck --check-prefix=CONTENTS %s
# RUN: llvm-nm %t.exe | FileCheck --check-prefix=NM %s

# The DLL's descriptor, and one for the plain slot in ro_mixed, now in .data.
# CHECK:      Import {
# CHECK-NEXT:   Name: import-slots-fill.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x2178
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT:   Symbol: variable (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: import-slots-fill.s.tmp.lib.dll
# CHECK-NEXT:   ImportLookupTableRVA:
# CHECK-NEXT:   ImportAddressTableRVA: 0x3010
# CHECK-NEXT:   Symbol: func (0)
# CHECK-NEXT: }
# CHECK-NOT:  Import {

# The load configuration's two table pointers and the two initializer table
# entries; neither slot with an offset.
# CHECK:      BaseReloc [
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x2080
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x20A0
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x4008
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x4010
# CHECK-NEXT:   }
# CHECK-NEXT: ]

# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x140001000
# CHECK-NEXT:   0x140001041
# CHECK-NEXT: ]
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x140003010
# CHECK-NEXT: ]

# Each slot: variable's address from the import address table, the offset,
# the store; then success.
# CODE:      140001000: movq 0x1179(%rip), %rax # 0x140002180
# CODE-NEXT: 140001007: movabsq $0x100000000, %rcx
# CODE-NEXT: 140001011: addq %rcx, %rax
# CODE-NEXT: 140001014: movq %rax, 0x1fe5(%rip) # 0x140003000 <rw_far>
# CODE-NEXT: 14000101b: movq 0x115e(%rip), %rax # 0x140002180
# CODE-NEXT: 140001022: addq $0x8, %rax
# CODE-NEXT: 140001028: movq %rax, 0x1fd9(%rip) # 0x140003008 <ro_mixed>
# CODE-NEXT: 14000102f: xorl %eax, %eax
# CODE-NEXT: 140001031: retq

# Both slots hold zero until then; the plain slot holds func's lookup entry.
# The initializer table: __xi_a, the fill function, user_init, __xi_z.
# CONTENTS:      Contents of section .data:
# CONTENTS-NEXT: 140003000 00000000 00000000 00000000 00000000
# CONTENTS-NEXT: 140003010 90210000 00000000
# CONTENTS:      Contents of section .CRT:
# CONTENTS-NEXT: 140004000 00000000 00000000 00100040 01000000
# CONTENTS-NEXT: 140004010 41100040 01000000 00000000 00000000

# NM-DAG: 140003000 D rw_far
# NM-DAG: 140003008 D ro_mixed

#--- lib.s
.text
.globl func
func:
  ret
.data
.globl variable
variable:
  .fill 32, 1, 0

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
.globl user_init
user_init:
  xorl %eax, %eax
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

# The C initializer table, with one initializer of the program's own.
.section .CRT$XIA,"dr"
.p2align 3
.globl __xi_a
__xi_a:
  .quad 0
.section .CRT$XIC,"dr"
.p2align 3
  .quad user_init
.section .CRT$XIZ,"dr"
.p2align 3
.globl __xi_z
__xi_z:
  .quad 0

# A read-only chunk with a slot holding an offset and a plain one.
.section .rdata,"dr",one_only,ro_mixed
.p2align 3
.globl ro_mixed
ro_mixed:
  .quad variable+8
  .quad func

# A writable slot, with an offset that needs 64 bits.
.data
.globl rw_far
rw_far:
  .quad variable+0x100000000

.section .gfids$y,"dr"
.symidx user_init
.section .giats$y,"dr"
.section .gljmp$y,"dr"
