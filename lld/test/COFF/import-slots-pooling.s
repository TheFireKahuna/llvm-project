# REQUIRES: x86

# Each destination run retains its own import descriptor. Immutable lookup
# sequences share only equal terminated tails, including ordinary lookups.
# Encounter the short tail before its longer sequence to check that input order
# does not prevent sharing. Named and ordinal imports use the same mechanism.
# Explicit roots retain the ordinary cells whose lookup tails are reused here.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/lib.s -o %t/lib.obj
# RUN: lld-link -dll -noentry -out:%t/provider.dll %t/lib.obj -export:a,DATA -export:b,DATA -export:c,DATA -export:z,@7,NONAME,DATA -implib:%t/provider.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/slots.s -o %t/slots.obj
# RUN: lld-link -dll -noentry -import-slots -include:__imp_a -include:__imp_b -include:__imp_c -include:__imp_z -out:%t/slots.dll %t/slots.obj %t/provider.lib
# RUN: llvm-readobj --coff-imports --coff-basereloc %t/slots.dll | FileCheck %s
# RUN: lld-link -dll -noentry -import-slots -timestamp:0 -include:__imp_a -include:__imp_b -include:__imp_c -include:__imp_z -out:%t/stable.dll %t/slots.obj %t/provider.lib
# RUN: cp %t/stable.dll %t/first.dll
# RUN: lld-link -dll -noentry -import-slots -timestamp:0 -include:__imp_a -include:__imp_b -include:__imp_c -include:__imp_z -out:%t/stable.dll %t/slots.obj %t/provider.lib
# RUN: cmp %t/first.dll %t/stable.dll

# RUN: llvm-dlltool -m i386 -d %t/provider32.def -l %t/provider32.lib
# RUN: llvm-mc -filetype=obj -triple=i686-windows-msvc %t/slots32.s -o %t/slots32.obj
# RUN: lld-link -dll -noentry -safeseh:no -import-slots -include:__imp__a -include:__imp__b -out:%t/slots32.dll %t/slots32.obj %t/provider32.lib
# RUN: llvm-readobj --coff-imports %t/slots32.dll | FileCheck %s --check-prefix=COFF32

# COFF32:      ImportLookupTableRVA: 0x[[#%X, ORDINARY32:]]
# COFF32-NEXT: ImportAddressTableRVA: 0x[[#ORDINARY32+24]]
# COFF32-NEXT: Symbol: a (0)
# COFF32-NEXT: Symbol: b (0)
# COFF32-NEXT: }
# COFF32:      ImportLookupTableRVA: 0x[[#%X, A32:]]
# COFF32-NEXT: ImportAddressTableRVA:
# COFF32-NEXT: Symbol: a (0)
# COFF32-NEXT: }
# COFF32:      ImportLookupTableRVA: 0x[[#A32-4]]
# COFF32-NEXT: ImportAddressTableRVA:
# COFF32-NEXT: Symbol: a (0)
# COFF32-NEXT: Symbol: a (0)
# COFF32-NEXT: }
# COFF32:      ImportLookupTableRVA: 0x[[#ORDINARY32+4]]
# COFF32-NEXT: ImportAddressTableRVA:
# COFF32-NEXT: Symbol: b (0)
# COFF32-NEXT: }
# COFF32-NOT: Import {

# Ordinary imports, sorted by symbol name.
# CHECK:      Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#%X, ORDINARY:]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#ORDINARY+88]]
# CHECK-NEXT:   Symbol: a (0)
# CHECK-NEXT:   Symbol: b (0)
# CHECK-NEXT:   Symbol: c (0)
# CHECK-NEXT:   Symbol:  (7)
# CHECK-NEXT: }

# [b, 0] is the tail of [a, b, 0], even though it appears first.
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#%X, B:]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#%X, DATA:]]
# CHECK-NEXT:   Symbol: b (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#B-8]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#DATA+16]]
# CHECK-NEXT:   Symbol: a (0)
# CHECK-NEXT:   Symbol: b (0)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#B-8]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#DATA+40]]
# CHECK-NEXT:   Symbol: a (0)
# CHECK-NEXT:   Symbol: b (0)
# CHECK-NEXT: }

# [a, 0] cannot alias the unterminated beginning of [a, b, 0].
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#%X, A:]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#DATA+64]]
# CHECK-NEXT:   Symbol: a (0)
# CHECK-NEXT: }

# [c, ordinal 7, 0] and [ordinal 7, 0] reuse the ordinary lookup tail.
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#ORDINARY+16]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#DATA+80]]
# CHECK-NEXT:   Symbol: c (0)
# CHECK-NEXT:   Symbol:  (7)
# CHECK-NEXT: }
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#ORDINARY+24]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#DATA+104]]
# CHECK-NEXT:   Symbol:  (7)
# CHECK-NEXT: }

# [b, a, 0] shares its tail with [a, 0], without changing either destination.
# CHECK-NEXT: Import {
# CHECK-NEXT:   Name: provider.dll
# CHECK-NEXT:   ImportLookupTableRVA: 0x[[#A-8]]
# CHECK-NEXT:   ImportAddressTableRVA: 0x[[#DATA+120]]
# CHECK-NEXT:   Symbol: b (0)
# CHECK-NEXT:   Symbol: a (0)
# CHECK-NEXT: }
# CHECK-NEXT: BaseReloc [
# CHECK-NEXT: ]

#--- lib.s
.data
.globl a, b, c, z
a: .quad 1
b: .quad 2
c: .quad 3
z: .quad 4

#--- slots.s
.data
.p2align 3
.quad b, 0
.quad a, b, 0
.quad a, b, 0
.quad a, 0
.quad c, z, 0
.quad z, 0
.quad b, a, 0

#--- provider32.def
LIBRARY provider32.dll
EXPORTS
a DATA
b DATA

#--- slots32.s
.data
.p2align 2
.long _a, 0
.long _a, _a, 0
.long _b, 0
