# REQUIRES: aarch64

## On ARM64, the adrp and ldr of __imp_X for an extern_weak X, which the object
## keeps a weak external, become the adrp and add of X once X is defined in
## the image, a move of zero once X is absent, and stay the adrp and ldr of X's
## pointer once an import library offers X: a cell in .sealed, which the
## image's fill, the builtins' __llvm_fill, writes when the image starts, null
## where the DLL lacks X, so that a DLL whose only imports are weak is not a
## dependency. No local import pointer is left, and none is reported.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium defs.s -o defs.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -dll -noentry -machine:arm64 -out:dll.dll -implib:dll.lib \
# RUN:   dll.obj -export:g
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj defs.obj crt.obj dll.lib ntdll.lib 2>&1 | count 0
# RUN: llvm-readobj --sections a.exe > a.txt
# RUN: llvm-objdump -d --no-show-raw-insn a.exe >> a.txt
# RUN: llvm-objdump -s -j .rdata a.exe >> a.txt
# RUN: FileCheck --input-file=a.txt %s
# RUN: llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=IMAGE %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x8
# CHECK-NEXT: VirtualAddress: 0x4000

## f resolves to its definition, absent to zero, and g to a load of its cell.
# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: adrp x2, 0x140004000
# CHECK-NEXT: ldr x2, [x2]
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

## __llvm_fill loads its list, whose address its adrp and add name.
# CHECK:      adrp x0, 0x[[#%x,LPAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,LOFF:]]

## The list: no cells, one run-time cell, no residual word; the section it
## seals is .sealed at 0x4000, size 8; then the run-time cell for g, {cell,
## name, dll, name length, dll length}, resolved in dll.dll.
# CHECK:      [[#%x,LPAGE+LOFF]]      00000000 00000000 01000000 00000000
# CHECK-NEXT: [[#%x,LPAGE+LOFF+0x10]] 00000000 00000000 00400000 08000000
# CHECK-NEXT: [[#%x,LPAGE+LOFF+0x20]] {{[0-9a-f]+ [0-9a-f]+}} 00000000 00000000
# CHECK-NEXT: [[#%x,LPAGE+LOFF+0x30]] 00400000 40200000 42200000 01000e00
# CHECK-NEXT: [[#%x,LPAGE+LOFF+0x40]] 67006400 6c006c00 2e006400 6c006c00  g.d.l.l...d.l.l.

# IMAGE-NOT: Name: dll.dll
# IMAGE:     Name: ntdll.dll
# IMAGE-NOT: Name: dll.dll

#--- main.s
  .text
  .globl main
main:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  adrp x1, __imp_absent
  ldr x1, [x1, :lo12:__imp_absent]
  adrp x2, __imp_g
  ldr x2, [x2, :lo12:__imp_g]
  ret

  .weak f
  .weak absent
  .weak g

#--- defs.s
  .text
  .globl f
f:
  ret

#--- dll.s
  .text
  .globl g
g:
  ret

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the ntdll functions it binds run-time imports and seals with.
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
  adrp x2, __imp_LdrGetDllHandleEx
  ldr x2, [x2, :lo12:__imp_LdrGetDllHandleEx]
  adrp x3, __imp_LdrGetProcedureAddressForCaller
  ldr x3, [x3, :lo12:__imp_LdrGetProcedureAddressForCaller]
  blr x1
  ret

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrGetDllHandleEx
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
