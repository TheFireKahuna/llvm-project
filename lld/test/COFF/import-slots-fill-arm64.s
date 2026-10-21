# REQUIRES: aarch64
## The ARM64 form of the image's fill, the builtins' __llvm_fill: each residual
## word of its list, {word, import, addend}, is written from its import's entry
## plus its addend, and the read-only ones move to .sealed, linked read-only
## behind the fill's done word, whose RVA and size the list names so the fill
## can open it around its writes once. The linker keeps __llvm_fill in the image
## only where it is needed, defines its list, and points the start-up's
## __llvm_run_fill at it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rw.s -o rw.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn out.dll > out.txt
# RUN: llvm-objdump -s -j .rdata out.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s
# RUN: llvm-readobj --sections out.dll | FileCheck --check-prefix=IMAGE %s

## __llvm_fill loads its list, whose address its adrp and add name.
# CHECK:      <.text>:
# CHECK-NEXT: adrp x0, 0x[[#%x,LIST:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
## The start-up calls the fill directly.
# CHECK:      bl 0x180001000 <.text>

## The list header: no ranges, no cells, three residual words, no run-time
## cells; the section it opens is .sealed at 0x5000, size 0x18; then the key
## of this DLL's name, and a flag word that runs no copy.
# CHECK:      [[#%x,LIST+OFF]]      00000000 00000000 03000000 00000000
# CHECK-NEXT: [[#%x,LIST+OFF+0x10]] 00500000 18000000 {{[0-9a-f]+ [0-9a-f]+}}
## The words follow the 40-byte header: two in .sealed after the done word,
## arr + 12 and arr - 8, and the one in .data, arr + 0x100002345, each from
## arr's entry at 0x20B8.
# CHECK-NEXT: [[#%x,LIST+OFF+0x20]] 00000000 00000000 08500000 b8200000
# CHECK-NEXT: [[#%x,LIST+OFF+0x30]] 0c000000 00000000 10500000 b8200000
# CHECK-NEXT: [[#%x,LIST+OFF+0x40]] f8ffffff ffffffff 00300000 b8200000
# CHECK-NEXT: [[#%x,LIST+OFF+0x50]] 45230000 01000000 {{.*}}

# IMAGE:      Name: .sealed
# IMAGE-NEXT: VirtualSize: 0x18
# IMAGE:      Characteristics [
# IMAGE-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# IMAGE-NEXT:   IMAGE_SCN_MEM_READ
# IMAGE-NEXT: ]

## With only writable words, the fill names no section to seal, and the image
## has no .sealed section.
# RUN: lld-link -dll -noentry -import-slots -out:leaf.dll crt.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn leaf.dll > leaf.txt
# RUN: llvm-objdump -s -j .rdata leaf.dll >> leaf.txt
# RUN: llvm-readobj --sections leaf.dll >> leaf.txt
# RUN: FileCheck --check-prefix=LEAF --input-file=leaf.txt %s

# LEAF:      adrp x0, 0x[[#%x,LLIST:]]
# LEAF-NEXT: add x0, x0, #0x[[#%x,LOFF:]]
## One residual word and no section to seal.
# LEAF:      [[#%x,LLIST+LOFF]]      00000000 00000000 01000000 00000000
# LEAF-NEXT: [[#%x,LLIST+LOFF+0x10]] 00000000 00000000 {{[0-9a-f]+ [0-9a-f]+}}
# LEAF-NOT:  Name: .sealed

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
  .xword 0x1111111111111111
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .xword 0

## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  bl __llvm_run_fill
  ret

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it seals with, so the linker adds that import.
  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  adrp x0, __llvm_fill_list
  add x0, x0, :lo12:__llvm_fill_list
  adrp x1, __imp_NtProtectVirtualMemory
  ldr x1, [x1, :lo12:__imp_NtProtectVirtualMemory]
  blr x1
  ret

#--- ro.s
  .section .rdata,"dr"
  .p2align 3
  .xword arr+12
  .xword arr-8

#--- rw.s
  .data
  .p2align 3
  .xword arr+0x100002345
