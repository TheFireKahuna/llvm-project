# REQUIRES: x86
## Under -import-slots, a word of static data that holds an address inside an
## import for which its DLL exports no name is written by the image's fill, the
## builtins' __llvm_fill, whose pointer the linker places in the C initializer
## table ahead of every initializer, from a list of {word, import, addend}
## entries. The linker keeps __llvm_fill in the image only where it is needed,
## defines its list, and names it to the table. A read-only word moves with its
## chunk, slots and all, to .sealed, whose RVA and size the list names so the
## fill can make it read-only once written; a writable word stays in place, and
## a fill with no read-only word names no section to seal.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tls.s -o tls.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium named.s -o named.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium cfg.s -o cfg.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-readobj --coff-imports out.dll > out.txt
# RUN: llvm-readobj --sections out.dll >> out.txt
# RUN: llvm-objdump -d --no-show-raw-insn out.dll >> out.txt
# RUN: llvm-objdump -s -j .rdata out.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s
# RUN: llvm-objdump -s -j .CRT out.dll | FileCheck --check-prefix=TABLE %s

## arr keeps its entry, which the fill reads, and its slot is a run of its own
## in .sealed. The fill imports NtProtectVirtualMemory to seal it.
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x20B8
# CHECK-NEXT: Symbol: arr
# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,PROTECT:]]
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x5008
# CHECK-NEXT: Symbol: arr

# CHECK:      Name: .data
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x3000
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x5000
# CHECK:      Characteristics [
# CHECK-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# CHECK-NEXT:   IMAGE_SCN_MEM_READ
# CHECK-NEXT:   IMAGE_SCN_MEM_WRITE
# CHECK-NEXT: ]

## __llvm_fill loads its list and seals .sealed through NtProtectVirtualMemory.
# CHECK:      <.text>:
# CHECK-NEXT: leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,PROTECT+0x180000000]]

## The list header: no ranges, no cells, two residual words, no run-time cells;
## the section it seals is .sealed at 0x5000, size 0x10; then the key of this
## DLL's name, and a flag word that runs no copy.
# CHECK:      [[#%x,LIST]]      00000000 00000000 02000000 00000000
# CHECK-NEXT: [[#%x,LIST+0x10]] 00500000 10000000 {{[0-9a-f]+ [0-9a-f]+}}
## The words follow the 40-byte header: the slot in .sealed, arr + 12, and the
## word in .data, arr + 0x100000000, each from arr's entry at 0x20B8.
# CHECK-NEXT: [[#%x,LIST+0x20]] 00000000 00000000 00500000 b8200000
# CHECK-NEXT: [[#%x,LIST+0x30]] 0c000000 00000000 00300000 b8200000
# CHECK-NEXT: [[#%x,LIST+0x40]] 00000000 01000000 {{.*}}

## The fill's pointer follows the table's start, ahead of the other initializer.
# TABLE:      11111111 11111111 00100080 01000000
# TABLE-NEXT: 22222222 22222222 33333333 33333333

## Control Flow Guard lets the table call it.
# RUN: lld-link -dll -noentry -import-slots -guard:cf -out:guard.dll crt.obj \
# RUN:   ro.obj cfg.obj lib.lib ntdll.lib
# RUN: llvm-readobj --coff-load-config guard.dll | \
# RUN:   FileCheck --check-prefix=GUARD %s

# GUARD:      GuardFidTable [
# GUARD-NEXT:   0x180001000
# GUARD-NEXT: ]

## With only writable words, the fill names no section to seal, and the image
## has no .sealed section.
# RUN: lld-link -dll -noentry -import-slots -out:leaf.dll crt.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn leaf.dll > leaf.txt
# RUN: llvm-objdump -s -j .rdata leaf.dll >> leaf.txt
# RUN: llvm-readobj --sections leaf.dll >> leaf.txt
# RUN: FileCheck --check-prefix=LEAF --input-file=leaf.txt %s

# LEAF:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LLIST:]]
## One residual word and no section to seal.
# LEAF:      [[#%x,LLIST]]      00000000 00000000 01000000 00000000
# LEAF-NEXT: [[#%x,LLIST+0x10]] 00000000 00000000 {{[0-9a-f]+ [0-9a-f]+}}
# LEAF-NOT:  Name: .sealed

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll ro.obj lib.lib \
# RUN:   ntdll.lib 2>&1 | FileCheck --check-prefix=NOTABLE %s
# RUN: not lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=NOPROTECT %s
# RUN: not lld-link -dll -noentry -import-slots -align:512 -out:out.dll \
# RUN:   crt.obj ro.obj lib.lib ntdll.lib 2>&1 | \
# RUN:   FileCheck --check-prefix=ALIGN %s
# RUN: not lld-link -dll -noentry -import-slots -out:out.dll crt.obj tls.obj \
# RUN:   named.obj lib.lib ntdll.lib 2>&1 | FileCheck --check-prefix=PLACE %s

# NOTABLE: error: ro.obj: .rdata holds the address of arr plus 12, imported from lib.dll, which exports no name for it, and the image has no C initializer table (__xi_a) to write it
## Without ntdll the fill's own reference to NtProtectVirtualMemory is undefined.
# NOPROTECT: error: undefined symbol: {{.*}}NtProtectVirtualMemory
# NOPROTECT: referenced by crt.obj:(__llvm_fill)
# ALIGN: error: ro.obj: .rdata holds the address of arr plus 12, imported from lib.dll, which exports no name for it, and its section alignment is smaller than a page, which the image cannot make read-only once written
# PLACE: error: tls.obj: .tls$ holds the address of arr plus 4, imported from lib.dll, which exports no name for it, in thread-local data
# PLACE: error: named.obj: .rdata$x holds the address of arr plus 4, imported from lib.dll, which exports no name for it, in a read-only section that cannot move

#--- lib.def
LIBRARY lib.dll
EXPORTS
  arr DATA

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0x1111111111111111
  .section .CRT$XIC,"dr"
  .quad 0x2222222222222222
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0x3333333333333333

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it seals with, so the linker adds that import.
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

#--- ro.s
  .section .rdata,"dr"
  .p2align 3
  .quad arr+12
  .quad arr

#--- rw.s
  .data
  .p2align 3
  .quad arr+0x100000000

#--- tls.s
  .section .tls$,"dw"
  .p2align 3
  .quad arr+4

#--- named.s
  .section .rdata$x,"dr"
  .p2align 3
  .quad arr+4

#--- cfg.s
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
  .quad __guard_longjmp_table
  .quad __guard_fids_count
  .fill 84, 1, 0
