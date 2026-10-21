# REQUIRES: x86
## Under -import-slots, __imp_X$val names a word of the image's own that its
## fill writes with the word import X holds, and __imp_X$init one it writes
## with what X returns when called with the image's base. Each is a word of
## .sealed, linked read-only and written under the fill's bracket, so code
## reads a value another image provides at run time in one load of read-only
## memory. The list names them as words whose import RVA carries the kind in
## its top two bits, 1 for a value and 2 for a resolver, every $init first,
## then every $val, each by name.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium other.s -o other.obj
# RUN: lld-link -def:prov.def -out:prov.lib -machine:x64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj prov.lib \
# RUN:   ntdll.lib -out:use.dll
# RUN: llvm-readobj --coff-imports --sections use.dll > use.txt
# RUN: llvm-objdump -d --no-show-raw-insn use.dll >> use.txt
# RUN: llvm-objdump -s -j .rdata use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s

## .sealed: the done word, then attach$init, base$val and map$val.
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x20
# CHECK-NEXT: VirtualAddress: 0x4000
# CHECK:      Characteristics [
# CHECK-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# CHECK-NEXT:   IMAGE_SCN_MEM_READ
# CHECK-NEXT: ]

## The provider's three imports keep their entries, which the fill reads.
# CHECK:      Name: prov.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x20C8
# CHECK-NEXT: Symbol: attach
# CHECK-NEXT: Symbol: base
# CHECK-NEXT: Symbol: map

## Code loads each value from its word.
# CHECK:      <.text>:
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x180004018
# CHECK-NEXT: movq {{.*}}(%rip), %rcx # 0x180004010
# CHECK-NEXT: movq {{.*}}(%rip), %rdx # 0x180004008
# CHECK-NEXT: retq
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]

## The list: no ranges or cells, three words, no run-time cells; .sealed at
## 0x4000; then, after the flag words, the words {word, import RVA | kind <<
## 30, addend}: attach's entry at 0x20C8 as a resolver, base's at 0x20D0 and
## map's at 0x20D8 as values.
# CHECK:      [[#%x,LIST]]      00000000 00000000 03000000 00000000
# CHECK-NEXT: [[#%x,LIST+0x10]] 00400000 20000000 {{[0-9a-f]+ [0-9a-f]+}}
# CHECK-NEXT: [[#%x,LIST+0x20]] 00000000 00000000 08400000 c8200080
# CHECK-NEXT: [[#%x,LIST+0x30]] 00000000 00000000 10400000 d0200040
# CHECK-NEXT: [[#%x,LIST+0x40]] 00000000 00000000 18400000 d8200040

## A sealed value whose name is not imported is refused.
# RUN: not lld-link -import-slots -dll -noentry other.obj crt.obj prov.lib \
# RUN:   ntdll.lib -out:other.dll 2>&1 | FileCheck --check-prefix=NOIMPORT %s
# NOIMPORT: error: the image reads __imp_absent$val, but absent is not imported

#--- use.s
  .text
  .globl f
f:
  movq __imp_map$val(%rip), %rax
  movq __imp_base$val(%rip), %rcx
  movq __imp_attach$init(%rip), %rdx
  retq

#--- other.s
  .text
  .globl g
g:
  movq __imp_absent$val(%rip), %rax
  retq

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it opens .sealed with.
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

#--- prov.def
LIBRARY prov.dll
EXPORTS
  attach
  base DATA
  map DATA

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory
