# REQUIRES: aarch64
## The AArch64 form of the executable's copy of its thread-local import slots,
## the builtins' __llvm_thread_local_slot_copy, which the linker makes the
## executable's first TLS callback. The linker builds its list, naming for each
## slot its RVA and the lookup entry the template holds for it; the copy reads
## the image's own TLS directory for the index and template, and the linker
## names the list in the MZ header for a DLL that may reach the executable first.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console tl.obj lib.lib \
# RUN:   -out:tl.exe
# RUN: llvm-readobj --coff-tls-directory tl.exe > tl.txt
# RUN: llvm-objdump -s -j .CRT tl.exe >> tl.txt
# RUN: llvm-objdump -s -j .rdata tl.exe >> tl.txt
# RUN: od -A x -t x4 -j 0x1c -N 8 tl.exe >> tl.txt
# RUN: FileCheck --input-file=tl.txt %s

## The template holds its index and start RVAs; the slot is at offset 8 in it.
# CHECK:      StartAddressOfRawData: 0x140005000
# CHECK:      AddressOfIndex: 0x140003000

## The TLS callback array, after its opening null, holds the copy's pointer.
# CHECK:      Contents of section .CRT:
# CHECK-NEXT: 00000000 00000000 {{[0-9a-f]+}} 01000000
# CHECK-NEXT: 00000000 00000000

## The copy list: one slot, its count a 64-bit word; then the slot's RVA, the
## template start plus 8, and the lookup entry 0x2078 the template holds for it.
# CHECK:      140002020 {{[0-9a-f]+ [0-9a-f]+}} 01000000 00000000
# CHECK-NEXT: 140002030 08500000 00000000 78200000 00000000

## The MZ header names the copy list: the tag 0x9E3779B9 and its RVA.
# CHECK:      9e3779b9 00002028

## A DLL that imports from an executable first runs the copy the executable
## names from its fill: the linker sets bit 0 of its fill list's flag word.
# RUN: lld-link -def:host.def -out:host.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium plug.s -o plug.obj
# RUN: lld-link -import-slots -dll -noentry plug.obj host.lib -out:plug.dll
# RUN: llvm-objdump -d --no-show-raw-insn plug.dll > plug.txt
# RUN: llvm-objdump -s -j .rdata plug.dll >> plug.txt
# RUN: FileCheck --check-prefix=CALL --input-file=plug.txt %s

## __llvm_fill loads its list, whose address its adrp and add name.
# CALL:      adrp x0, 0x[[#%x,PAGE:]]
# CALL-NEXT: add x0, x0, #0x[[#%x,OFF:]]
## One run-time cell, the cell in .sealed at 0x4000; the flag word's bit 0 set
## to run the executable's copy; the run-time cell for hostfn, bound in the
## executable (dll word 0), whose name follows the list.
# CALL:      [[#%x,PAGE+OFF]]      00000000 00000000 00000000 01000000
# CALL-NEXT: [[#%x,PAGE+OFF+0x10]] 00400000 08000000 {{[0-9a-f]+ [0-9a-f]+}}
# CALL-NEXT: [[#%x,PAGE+OFF+0x20]] 01000000 00000000 00400000 {{[0-9a-f]+}}
# CALL-NEXT: [[#%x,PAGE+OFF+0x30]] 00000000 {{.*}}

#--- host.def
LIBRARY host.exe
EXPORTS
  hostfn

#--- plug.s
  .text
  .globl dllmain
dllmain:
  b hostfn
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
  adrp x0, __llvm_fill_list
  add x0, x0, :lo12:__llvm_fill_list
  ret

#--- lib.def
LIBRARY lib.dll
EXPORTS
  func

#--- tl.s
  .text
  .globl main
main:
  ret
  .section .tls,"dw"
  .globl _tls_start
_tls_start:
  .quad 0
  .section .tls$,"dw"
  .p2align 3
  .quad func
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
  .section .CRT$XLA,"dr"
  .globl __xl_a
__xl_a:
  .quad 0
  .section .CRT$XLZ,"dr"
  .globl __xl_z
__xl_z:
  .quad 0

  .def __llvm_thread_local_slot_copy
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_thread_local_slot_copy
  .globl __llvm_thread_local_slot_copy
__llvm_thread_local_slot_copy:
  adrp x0, __llvm_thread_local_slot_copy_list
  add x0, x0, :lo12:__llvm_thread_local_slot_copy_list
  ret
