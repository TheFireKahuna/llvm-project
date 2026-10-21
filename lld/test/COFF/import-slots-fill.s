# REQUIRES: x86
## Under -import-slots, a word of static data that holds an address inside an
## import for which its DLL exports no name is written by the image's fill, the
## builtins' __llvm_fill, from a list of {word, import, addend} entries. The
## image's start-up calls it directly, as __llvm_run_fill, which the linker
## defines as __llvm_fill where the image needs a fill and as a function
## returning 0 where it does not; the linker keeps __llvm_fill only where it is
## needed and defines its list. A read-only word moves with its chunk, slots and
## all, to .sealed, which is linked read-only and opens with the fill's done
## word, and whose RVA and size the list names so the fill can open it around
## its writes once; a writable word stays in place, and a fill with no
## read-only word names no section to seal. A word of another read-only
## section, or of thread-local data, stays in place too.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tls.s -o tls.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium named.s -o named.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium cfg.s -o cfg.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium take.s -o take.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tlsdir.s \
# RUN:   -o tlsdir.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-readobj --coff-imports out.dll > out.txt
# RUN: llvm-readobj --sections out.dll >> out.txt
# RUN: llvm-objdump -d --no-show-raw-insn out.dll >> out.txt
# RUN: llvm-objdump -s -j .rdata out.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s

## arr keeps its entry, which the fill reads, and its slot is a run of its own
## in .sealed, which leads the import address table directory so the loader
## writes the slot while it binds. The fill imports NtProtectVirtualMemory to
## open .sealed around its writes.
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x3000
# CHECK-NEXT: Symbol: arr
# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,PROTECT:]]
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x2010
# CHECK-NEXT: Symbol: arr

## .sealed is linked read-only: the done word at 0x2000, then the chunk.
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x18
# CHECK-NEXT: VirtualAddress: 0x2000
# CHECK:      Characteristics [
# CHECK-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# CHECK-NEXT:   IMAGE_SCN_MEM_READ
# CHECK-NEXT: ]
# CHECK:      Name: .data
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x4000

## __llvm_fill loads its list and opens .sealed through NtProtectVirtualMemory;
## the start-up calls it directly.
# CHECK:      <.text>:
# CHECK-NEXT: leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,PROTECT+0x180000000]]
# CHECK:      callq 0x180001000 <.text>

## The list header: no ranges, no cells, two residual words, no run-time cells;
## the section it opens is .sealed at 0x2000, size 0x18; then the key of this
## DLL's name, and a flag word that runs no copy.
# CHECK:      [[#%x,LIST]]      00000000 00000000 02000000 00000000
# CHECK-NEXT: [[#%x,LIST+0x10]] 00200000 18000000 {{[0-9a-f]+ [0-9a-f]+}}
## The words follow the 40-byte header: the one in .sealed, arr + 12, and the
## one in .data, arr + 0x100000000, each from arr's entry at 0x3000.
# CHECK-NEXT: [[#%x,LIST+0x20]] 00000000 00000000 08200000 00300000
# CHECK-NEXT: [[#%x,LIST+0x30]] 0c000000 00000000 00400000 00300000
# CHECK-NEXT: [[#%x,LIST+0x40]] 00000000 01000000 {{.*}}

## Only the start-up's direct call reaches the fill, so Control Flow Guard
## lists it as no target, and an image that takes its address is refused.
# RUN: lld-link -dll -noentry -import-slots -guard:cf -out:guard.dll crt.obj \
# RUN:   ro.obj cfg.obj lib.lib ntdll.lib
# RUN: llvm-readobj --coff-load-config guard.dll | \
# RUN:   FileCheck --check-prefix=GUARD %s
# RUN: not lld-link -dll -noentry -import-slots -guard:cf -out:out.dll crt.obj \
# RUN:   ro.obj cfg.obj take.obj lib.lib ntdll.lib 2>&1 | \
# RUN:   FileCheck --check-prefix=TAKEN %s

# GUARD: GuardCFFunctionCount: 0
# TAKEN: error: the image takes the address of its fill, __llvm_fill, which only its start-up may call, and only directly

## An image that needs no fill calls a function returning 0 in its place.
# RUN: lld-link -dll -noentry -import-slots -include:__start -out:none.dll \
# RUN:   crt.obj lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn none.dll | \
# RUN:   FileCheck --check-prefix=NONE %s

# NONE:      180001000: xorl %eax, %eax
# NONE-NEXT: retq
# NONE:      callq 0x180001000

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
# RUN:   lib.lib ntdll.lib 2>&1 | FileCheck --check-prefix=PLACE %s

# NOTABLE: error: ro.obj: .rdata holds the address of arr plus 12, imported from lib.dll, which exports no name for it, and the image's start-up does not call __llvm_run_fill to write it
## Without ntdll the fill's own reference to NtProtectVirtualMemory is undefined.
# NOPROTECT: error: undefined symbol: {{.*}}NtProtectVirtualMemory
# NOPROTECT: referenced by crt.obj:(__llvm_fill)
# ALIGN: error: the image's fill writes read-only memory, which its section alignment, smaller than a page, cannot keep apart
# PLACE: error: tls.obj: .tls$ holds the address of arr plus 4, imported from lib.dll, which exports no name for it, in thread-local data that the image has no TLS directory (_tls_used) for

## A word of a read-only section whose order its users may rely on stays
## where it is, and the fill makes its output section's range writable around
## the write; a fill that writes read-only memory has a .sealed of its own for
## its done word. A word of the thread-local template is written in the
## template and the loading thread's block too; the list's last word counts
## those, which follow the others.
# RUN: lld-link -dll -noentry -import-slots -out:place.dll crt.obj tls.obj \
# RUN:   named.obj tlsdir.obj lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn place.dll > place.txt
# RUN: llvm-objdump -s -j .rdata place.dll >> place.txt
# RUN: FileCheck --check-prefix=INPLACE --input-file=place.txt %s

# INPLACE:      leaq {{.*}}(%rip), %rax # 0x[[#%x,PLIST:]]
## One range, no cells, two words; .sealed, holding the done word alone, at
## 0x5000; then no flag and one thread-local word. The range is the word of
## .rdata$x, which stays in .rdata at 0x2080, and the words are it and the
## template's at 0x4000, each arr + 4 from arr's entry.
# INPLACE:      [[#%x,PLIST]]      01000000 00000000 02000000 00000000
# INPLACE-NEXT: [[#%x,PLIST+0x10]] 00500000 08000000 {{[0-9a-f]+ [0-9a-f]+}}
# INPLACE-NEXT: [[#%x,PLIST+0x20]] 00000000 01000000 80200000 08000000
# INPLACE-NEXT: [[#%x,PLIST+0x30]] 80200000 [[ENTRY:[0-9a-f]{8}]] 04000000 00000000
# INPLACE-NEXT: [[#%x,PLIST+0x40]] 00400000 [[ENTRY]] 04000000 00000000

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

## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq

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

#--- tlsdir.s
  .section .rdata,"dr"
  .p2align 3
  .globl _tls_used
_tls_used:
  .quad 0, 0, 0, 0
  .long 0, 0

#--- take.s
  .data
  .p2align 3
  .quad __llvm_fill

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
