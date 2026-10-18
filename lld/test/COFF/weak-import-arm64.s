# REQUIRES: aarch64

## On ARM64, the adrp and ldr of __imp_X for an extern_weak X, which the object
## keeps a weak external, become the adrp and add of X once X is defined in
## the image, a move of zero once X is absent, and stay the adrp and ldr of X's
## pointer once an import library offers X: a cell in .sealed, which the
## image's fill writes when the image starts, null where the DLL lacks X, so
## that a DLL whose only imports are weak is not a dependency. No local import
## pointer is left, and none is reported.

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
# CHECK-NEXT: VirtualAddress: 0x5000
# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: adrp x2, 0x140005000
# CHECK-NEXT: ldr x2, [x2]
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

## For each import of the list, the fill finds dll.dll among the loaded
## modules, its name a UNICODE_STRING at sp + 0x70, leaving the cell null if
## it is not there; then the import in it, its name an ANSI_STRING there, with
## flag 1, which binds without initializing the DLL. A host import that is
## not weak and is missing fails the fill.
# CHECK:      adrp x19, 0x[[#%x,LPAGE:]]
# CHECK-NEXT: add x19, x19, #0x[[#%x,LOFF:]]
# CHECK-NEXT: adrp x20,
# CHECK-NEXT: add x20, x20, #0x[[#%x,LOFF+0x10]]
# CHECK-NEXT: cmp x19, x20
# CHECK-NEXT: b.hs
# CHECK-NEXT: ldr w9, [x19, #0x8]
# CHECK-NEXT: cmp w9, #0x1
# CHECK-NEXT: b.ls 0x[[#%x,HOST:]]
# CHECK-NEXT: add x16, x27, x9
# CHECK-NEXT: str x16, [sp, #0x78]
# CHECK-NEXT: ldrh w9, [x19, #0xe]
# CHECK-NEXT: add w10, w9, #0x2
# CHECK-NEXT: orr w9, w9, w10, lsl #16
# CHECK-NEXT: str w9, [sp, #0x70]
# CHECK-NEXT: mov w0, #0x1
# CHECK-NEXT: mov x1, xzr
# CHECK-NEXT: mov x2, xzr
# CHECK-NEXT: add x3, sp, #0x70
# CHECK-NEXT: add x4, sp, #0x80
# CHECK-NEXT: adrp x16,
# CHECK-NEXT: ldr x16,
# CHECK-NEXT: blr x16
# CHECK-NEXT: tbnz w0, #0x1f, 0x[[#%x,NEXT:]]
# CHECK-NEXT: ldr x0, [sp, #0x80]
# CHECK-NEXT: b 0x[[#%x,LOOKUP:]]
# CHECK-NEXT: [[#%x,HOST]]: ldr x0, [x18, #0x60]
# CHECK-NEXT: ldr x0, [x0, #0x10]
# CHECK-NEXT: [[#%x,LOOKUP]]: ldr w9, [x19, #0x4]
# CHECK-NEXT: add x16, x27, x9
# CHECK-NEXT: str x16, [sp, #0x78]
# CHECK-NEXT: ldrh w9, [x19, #0xc]
# CHECK-NEXT: add w10, w9, #0x1
# CHECK-NEXT: orr w9, w9, w10, lsl #16
# CHECK-NEXT: str w9, [sp, #0x70]
# CHECK-NEXT: add x1, sp, #0x70
# CHECK-NEXT: mov w2, #0x0
# CHECK-NEXT: ldr w9, [x19]
# CHECK-NEXT: add x3, x27, x9
# CHECK-NEXT: mov w4, #0x1
# CHECK-NEXT: adrp x5, 0x140001000
# CHECK-NEXT: add x5, x5, #0x20
# CHECK-NEXT: adrp x16,
# CHECK-NEXT: ldr x16,
# CHECK-NEXT: blr x16
# CHECK-NEXT: ldr w9, [x19, #0x8]
# CHECK-NEXT: cbnz w9, 0x[[#%x,NEXT]]
# CHECK-NEXT: tbnz w0, #0x1f,
# CHECK-NEXT: [[#%x,NEXT]]: add x19, x19, #0x10

## The import, {cell, name, the DLL's name, the names' lengths}: g, from
## dll.dll.
# CHECK:      [[#%x,LPAGE+LOFF]] 00500000 10200000 12200000 01000e00
# CHECK-NEXT: 67006400 6c006c00 2e006400 6c006c00  g.d.l.l...d.l.l.

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

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrGetDllHandleEx
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
