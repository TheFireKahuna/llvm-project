# REQUIRES: aarch64

## On ARM64, the adrp and ldr of __imp_X for an extern_weak X, which the object
## keeps a weak external, become the adrp and add of X once X is defined in
## the image, a move of zero once X is absent, and stay the adrp and ldr of X's
## pointer once an import library offers X: a cell in .sealed, which the
## residual fill writes when the image starts, null where the DLL lacks X, so
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
# RUN: FileCheck --input-file=a.txt %s
# RUN: llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=IMAGE %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x8
# CHECK-NEXT: VirtualAddress: 0x[[#%x,SEALED:]]
# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: mov x1, #0x0
# CHECK-NEXT: adrp x2, 0x[[#SEALED + 0x140000000]]
# CHECK-NEXT: ldr x2, [x2]
# CHECK-NEXT: ret
# CHECK:      [[#PAGE + OFF]]: ret

## The fill finds dll.dll among the loaded modules and g in it.
# CHECK:      stp x29, x30, [sp, #-0x30]!
# CHECK-NEXT: mov x29, sp
# CHECK:      mov w0, #0x1
# CHECK:      blr x16
# CHECK-NEXT: tbnz w0, #0x1f,
# CHECK-NEXT: ldr x0, [sp, #0x20]

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
