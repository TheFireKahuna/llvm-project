# REQUIRES: x86
## Under -import-slots, a DLL's cells of an overridable default it imports are
## the words through which it reaches the default: each in-place import slot of
## it, and its import's entry, through which the DLL calls it and loads its
## address. The loader writes them with the runtime's entry. The DLL's fill, the
## builtins' __llvm_fill, which its C initializer table runs before every other
## initializer, writes the program's replacement over each cell of a replaced
## slot, from a list the linker builds: the cells ordered by key, each {key u64
## (bit 0 the read-only mark), rva u32, kind u32}, preceded by the read-only
## ranges it makes writable. The executable binds its own references at link
## time and carries no fill.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib

## The DLL calls the default, and holds its address in writable and in
## read-only data. The read-only word is the slot its import is read from, so
## the call's thunk reads it too. The fill imports NtProtectVirtualMemory.
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:use.dll
# RUN: llvm-readobj --coff-imports use.dll > use.txt
# RUN: llvm-objdump -d --print-imm-hex use.dll >> use.txt
# RUN: llvm-objdump -s -j .rdata use.dll >> use.txt
# RUN: FileCheck --check-prefixes=IMPORT,FILL --input-file=use.txt %s
# IMPORT:      Name: rt.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA: 0x20C0
# IMPORT-NEXT: Symbol: _Znwy
# IMPORT:      Name: ntdll.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA: 0x20D0
# IMPORT-NEXT: Symbol: NtProtectVirtualMemory
# IMPORT:      Name: rt.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA: 0x3000
# IMPORT-NEXT: Symbol: _Znwy

## __llvm_fill loads its list.
# FILL:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]

## The list header: one read-only range, two cells; no section to seal; then
## the key of this DLL's name, and a flag word that runs no copy. The range
## spans the read-only cell, the import entry at 0x20C0.
# FILL:      [[#%x,LIST]]      01000000 02000000 00000000 00000000
# FILL-NEXT: [[#%x,LIST+0x10]] 00000000 00000000 {{[0-9a-f]+ [0-9a-f]+}}
## Then, after the 40-byte header, the range, {rva, size}, and the cells, {key,
## rva, kind}: the writable one in .data, and the read-only import entry whose
## key has bit 0 set, each of slot 0, whose RVA is at 0x44 in the executable's
## header, each of kind 2.
# FILL-NEXT: [[#%x,LIST+0x20]] 00000000 00000000 c0200000 08000000
# FILL-NEXT: [[#%x,LIST+0x30]] 00000000 44000000 00300000 02000000
# FILL-NEXT: [[#%x,LIST+0x40]] 01000000 44000000 c0200000 02000000

## The C initializer table holds the fill's pointer right after the table's
## start.
# RUN: llvm-objdump -s -j .CRT use.dll | FileCheck --check-prefix=CRT %s
# CRT:      Contents of section .CRT:
# CRT-NEXT: 00000000 00000000 {{[0-9a-f]+}} 01000000

## With only a writable cell, which is then the slot its import is read from,
## the fill names no read-only range.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: lld-link -import-slots -dll -noentry rw.obj crt.obj rt.lib ntdll.lib \
# RUN:   -out:rw.dll
# RUN: llvm-objdump -d --print-imm-hex rw.dll > rw.txt
# RUN: llvm-objdump -s -j .rdata rw.dll >> rw.txt
# RUN: FileCheck --check-prefix=LEAF --input-file=rw.txt %s
# LEAF:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LLIST:]]
## No range, one cell.
# LEAF:      [[#%x,LLIST]] 00000000 01000000 00000000 00000000

## A DLL that reaches no overridable default carries no fill, so nothing
## references NtProtectVirtualMemory.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plain.s -o plain.obj
# RUN: lld-link -import-slots -dll -noentry plain.obj crt.obj rt.lib ntdll.lib \
# RUN:   -out:plain.dll
# RUN: llvm-readobj --coff-imports plain.dll | FileCheck --check-prefix=NONE %s
# NONE-NOT: NtProtectVirtualMemory

## Without a C initializer table nothing runs the fill, and without
## NtProtectVirtualMemory the fill's own reference to it is undefined: either is
## an error, not a silently unreplaced default.
# RUN: not lld-link -import-slots -dll -noentry use.obj rt.lib \
# RUN:   ntdll.lib -out:nocrt.dll 2>&1 | FileCheck --check-prefix=NOCRT %s
# NOCRT: error: the image reaches __declspec(dllimport) operator new(unsigned long long), an overridable default imported from rt.dll, through a cell that the image has no C initializer table (__xi_a) to fill
# RUN: not lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib \
# RUN:   -out:noprotect.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=NOPROTECT %s
# NOPROTECT: error: undefined symbol: {{.*}}NtProtectVirtualMemory
# NOPROTECT: referenced by crt.obj:(__llvm_fill)

## A thread-local cell is a slot in the template of the DLL's thread-local data.
## The fill reads the image's TLS directory for the index and template, and the
## cell's kind is a slot in the template (fillCellSlot | fillCellThreadLocal = 6).
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -dll -noentry tl.obj crt.obj rt.lib ntdll.lib \
# RUN:   -out:tl.dll
# RUN: llvm-readobj --coff-imports --sections tl.dll > tl.txt
# RUN: llvm-objdump -d --print-imm-hex tl.dll >> tl.txt
# RUN: llvm-objdump -s -j .rdata tl.dll >> tl.txt
# RUN: FileCheck --check-prefix=TL --input-file=tl.txt %s
# TL:      Name: .data
# TL-NEXT: VirtualSize:
# TL-NEXT: VirtualAddress: 0x3000
# TL:      Name: .tls
# TL-NEXT: VirtualSize:
# TL-NEXT: VirtualAddress: 0x5000
# TL:      Name: rt.dll
# TL-NEXT: ImportLookupTableRVA:
# TL-NEXT: ImportAddressTableRVA: 0x5008
# TL-NEXT: Symbol: _Znwy
# TL:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]
## One cell, a slot in the template at 0x5008, of kind 6.
# TL:      [[#%x,LIST]]      00000000 01000000 00000000 00000000
# TL-NEXT: [[#%x,LIST+0x10]] 00000000 00000000 {{[0-9a-f]+ [0-9a-f]+}}
# TL-NEXT: [[#%x,LIST+0x20]] 00000000 00000000 00000000 44000000
# TL-NEXT: [[#%x,LIST+0x30]] 08500000 06000000 {{.*}}

## An executable binds its own references at link time and carries no fill.
# RUN: lld-link -import-slots -entry:dllmain -subsystem:console use.obj crt.obj \
# RUN:   rt.lib ntdll.lib -out:use.exe
# RUN: llvm-readobj --coff-imports use.exe | FileCheck --check-prefix=NONE %s

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        retq
        .globl other
other:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy /EXPORT:other"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
## The C initializer table's bounds, as the C runtime provides them, so the
## linker names the fill to the table the runtime's start-up runs, and a
## stand-in for the builtins' __llvm_fill: it loads its list and references the
## NtProtectVirtualMemory it makes read-only cells writable with.
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .quad 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .quad 0

        .def __llvm_fill
        .scl 2
        .type 32
        .endef
        .section .text,"xr",one_only,__llvm_fill
        .globl __llvm_fill
__llvm_fill:
        leaq __llvm_fill_list(%rip), %rax
        callq *__imp_NtProtectVirtualMemory(%rip)
        retq

#--- use.s
        .text
        .globl dllmain
dllmain:
        callq _Znwy
        retq
        .data
        .globl gnew
gnew:
        .quad _Znwy
        .section .rdata,"dr",discard,cnew
        .globl cnew
        .p2align 3
cnew:
        .quad _Znwy

#--- rw.s
        .text
        .globl dllmain
dllmain:
        retq
        .data
        .globl gnew
gnew:
        .quad _Znwy

#--- plain.s
        .text
        .globl dllmain
dllmain:
        callq other
        retq
        .data
        .globl gother
gother:
        .quad other

#--- tl.s
        .text
        .globl dllmain
dllmain:
        retq
## The template starts with another thread-local datum, so the cell is at
## offset 8 in each thread's block.
        .section .tls,"dw"
        .globl _tls_start
_tls_start:
        .quad 0
        .section .tls$,"dw"
        .globl tnew
        .p2align 3
tnew:
        .quad _Znwy
        .section .tls$ZZZ,"dw"
        .globl _tls_end
_tls_end:
        .quad 0
        .data
        .globl _tls_index
_tls_index:
        .long 0
        .section .rdata,"dr"
        .globl _tls_used
        .p2align 3
_tls_used:
        .quad _tls_start
        .quad _tls_end
        .quad _tls_index
        .quad 0
        .long 0
        .long 0
