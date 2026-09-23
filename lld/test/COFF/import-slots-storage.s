# REQUIRES: x86

# Reuse immutable in-place storage for whole-pointer reads. Keep import aliases
# at the selected interior offset, and retain separate cells for observable
# cell addresses, roots and unknown uses.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/lib.s -o %t/lib.obj
# RUN: lld-link -dll -noentry -out:%t/provider.dll %t/lib.obj -export:variable,DATA -export:func -implib:%t/provider.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/base.s -o %t/base.obj
# RUN: lld-link -dll -noentry -import-slots -debug:symtab -map:%t/only.map -out:%t/only.dll %t/base.obj %t/provider.lib
# RUN: llvm-readobj --file-headers --coff-imports %t/only.dll | FileCheck %s --check-prefix=ONLY
# RUN: llvm-nm %t/only.dll | FileCheck %s --check-prefix=NM
# RUN: FileCheck %s --check-prefix=MAP < %t/only.map
# RUN: lld-link -dll -noentry -import-slots -debug -pdb:%t/publics.pdb -out:%t/publics.dll %t/base.obj %t/provider.lib
# RUN: llvm-pdbutil dump -publics %t/publics.pdb | FileCheck %s --check-prefix=PDB

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/read.s -o %t/read.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/read.dll %t/base.obj %t/read.obj %t/provider.lib
# RUN: llvm-readobj --file-headers --coff-imports %t/read.dll | FileCheck %s --check-prefix=ONLY
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/address.s -o %t/address.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/address.dll %t/base.obj %t/address.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/address.dll | FileCheck %s --check-prefixes=VARIABLE,SLOTS
# RUN: lld-link -dll -noentry -import-slots -include:__imp_variable -out:%t/root.dll %t/base.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/root.dll | FileCheck %s --check-prefixes=VARIABLE,SLOTS
# RUN: lld-link -dll -noentry -import-slots -include:weak_cell -alternatename:weak_cell=__imp_variable -out:%t/alias-root.dll %t/base.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/alias-root.dll | FileCheck %s --check-prefixes=VARIABLE,SLOTS
# RUN: lld-link -dll -noentry -import-slots -export:cell=__imp_variable,DATA -out:%t/export.dll %t/base.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/export.dll | FileCheck %s --check-prefixes=VARIABLE,SLOTS
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/alias.s -o %t/alias.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/alias.dll %t/base.obj %t/alias.obj %t/provider.lib
# RUN: llvm-readobj --file-headers --coff-imports %t/alias.dll | FileCheck %s --check-prefix=ONLY

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/call.s -o %t/call.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/call.dll %t/base.obj %t/call.obj %t/provider.lib
# RUN: llvm-readobj --file-headers --coff-imports %t/call.dll | FileCheck %s --check-prefix=ONLY

# Neither a dead code reference nor a retained pointer read needs another cell
# when the immutable destination already survives independently.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/dead.s -o %t/dead.obj
# RUN: lld-link -dll -noentry -import-slots -opt:ref -out:%t/gc.dll %t/base.obj %t/dead.obj %t/provider.lib
# RUN: llvm-readobj --file-headers --coff-imports %t/gc.dll | FileCheck %s --check-prefix=ONLY
# RUN: lld-link -dll -noentry -import-slots -opt:noref -out:%t/nogc.dll %t/base.obj %t/dead.obj %t/provider.lib
# RUN: llvm-readobj --file-headers --coff-imports %t/nogc.dll | FileCheck %s --check-prefix=ONLY

# Writable and unaligned fields cannot replace an aligned immutable cell.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/writable.s -o %t/writable.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/writable.dll %t/writable.obj %t/read.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/writable.dll | FileCheck %s --check-prefix=TWO
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/unaligned.s -o %t/unaligned.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/unaligned.dll %t/unaligned.obj %t/read.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/unaligned.dll | FileCheck %s --check-prefix=TWO
# A preceding writable destination does not hide a later suitable immutable one.
# RUN: lld-link -dll -noentry -import-slots -out:%t/prefer.dll %t/writable.obj %t/base.obj %t/read.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/prefer.dll | FileCheck %s --check-prefix=TWO
# Narrow reads and cell-address materialization retain the ordinary cell.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/narrow.s -o %t/narrow.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/narrow.dll %t/base.obj %t/narrow.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/narrow.dll | FileCheck %s --check-prefixes=VARIABLE,SLOTS
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/lea.s -o %t/lea.obj
# RUN: lld-link -dll -noentry -import-slots -out:%t/lea.dll %t/base.obj %t/lea.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports %t/lea.dll | FileCheck %s --check-prefixes=VARIABLE,SLOTS

# Final layout must preserve the protection used to justify storage reuse.
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -import-slots -section:.rdata,RW -out:%t/rw.dll %t/base.obj %t/read.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=RW
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -import-slots -merge:.rdata=.text -out:%t/rx.dll %t/base.obj %t/read.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=RX
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -import-slots -section:.data,R -out:%t/outside.dll %t/writable.obj %t/read.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=OUTSIDE
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -import-slots -align:512 -out:%t/pages.dll %t/base.obj %t/read.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=PAGES

# TWO-COUNT-2: Symbol: variable (0)
# TWO-NOT: Symbol: variable (0)
# RW: immutable import storage must remain read-only in .rdata
# RX: native data import destination is executable in .text
# OUTSIDE: read-only import destination is outside the final import address table span
# PAGES: import address table protection span crosses section .text with incompatible permissions

# The whole read-only object occupies the IAT protection span, while the actual
# native destination starts one word into it. No ordinary cells or terminators.
# ONLY:      IATRVA: 0x[[#%X, IAT:]]
# ONLY-NEXT: IATSize: 0x20
# ONLY:      Import {
# ONLY-NEXT:   Name: provider.dll
# ONLY-NEXT:   ImportLookupTableRVA:
# ONLY-NEXT:   ImportAddressTableRVA: 0x[[#IAT+8]]
# ONLY-NEXT:   Symbol: variable (0)
# ONLY-NEXT:   Symbol: func (0)
# ONLY-NEXT: }
# ONLY-NOT: Import {

# NM-DAG: [[#%x, ADDRESS:]] R __imp_func
# NM-DAG: [[#ADDRESS-8]] R slots

# MAP:      0002:[[#%.8X, MAPOFFSET:]] slots
# MAP:      0002:[[#MAPOFFSET]] __imp_variable
# MAP-NEXT: 0002:[[#MAPOFFSET+8]] __imp_func

# PDB:      S_PUB32 {{.*}} `slots`
# PDB-NEXT: flags = none, addr = 0002:[[#%.4u, PDBOFFSET:]]
# PDB:      S_PUB32 {{.*}} `__imp_variable`
# PDB-NEXT: flags = none, addr = 0002:[[#PDBOFFSET]]
# PDB:      S_PUB32 {{.*}} `__imp_func`
# PDB-NEXT: flags = none, addr = 0002:[[#PDBOFFSET+8]]

# VARIABLE:      Import {
# VARIABLE-NEXT:   Name: provider.dll
# VARIABLE-NEXT:   ImportLookupTableRVA:
# VARIABLE-NEXT:   ImportAddressTableRVA:
# VARIABLE-NEXT:   Symbol: variable (0)
# VARIABLE-NEXT: }
# SLOTS-NEXT: Import {
# SLOTS-NEXT:   Name: provider.dll
# SLOTS-NEXT:   ImportLookupTableRVA:
# SLOTS-NEXT:   ImportAddressTableRVA:
# SLOTS-NEXT:   Symbol: variable (0)
# SLOTS-NEXT:   Symbol: func (0)
# SLOTS-NEXT: }
# SLOTS-NOT: Import {

#--- lib.s
.text
.globl func
func:
  ret
.data
.globl variable
variable:
  .quad 7

#--- base.s
.text
.globl main
main:
  ret
.section .rdata,"dr"
.p2align 3
.quad 0x123456789abcdef0
.globl slots
slots:
.quad variable, func
.quad 0xfedcba9876543210

#--- read.s
.text
  movq __imp_variable(%rip), %rax
  ret

#--- address.s
.data
  .quad __imp_variable

#--- alias.s
.weak weak_cell
.set weak_cell, __imp_variable
.text
  movq weak_cell(%rip), %rax
  ret

#--- call.s
.text
  jmp func

#--- dead.s
.section .text,"xr",discard,dead_read
.globl dead_read
dead_read:
  movq __imp_variable(%rip), %rax
  ret

#--- writable.s
.data
.p2align 3
.quad variable

#--- unaligned.s
.section .rdata,"dr"
.byte 1
.quad variable

#--- narrow.s
.text
  movl __imp_variable(%rip), %eax
  ret

#--- lea.s
.text
  leaq __imp_variable(%rip), %rax
  ret
