# REQUIRES: aarch64
## On ARM64, a function that the executable of a joint link defines takes the
## place of a member DLL's definition in the DLL's own references: its branch
## reaches the import's thunk and its adrp and add load the import, whose cell
## the fill writes from the executable. Another member imports it from the
## executable too, which exports it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium lib.s -o lib.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium user.s -o user.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium app.s -o app.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64

# RUN: lld-link -machine:arm64 -import-slots -dll -noentry lib.obj crt.obj \
# RUN:   ntdll.lib -out:lib.dll -lldjointscan:lib.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry user.obj crt.obj \
# RUN:   lib.lib ntdll.lib -out:user.dll -lldjointscan:user.jscan
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   app.obj lib.lib user.lib -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -lldjointmerge -out:facts lib.jscan user.jscan app.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry lib.obj crt.obj \
# RUN:   ntdll.lib -out:lib.dll -lldjoint:facts/lib.dll.jfacts
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry user.obj crt.obj \
# RUN:   lib.lib ntdll.lib -out:user.dll -lldjoint:facts/user.dll.jfacts
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   app.obj lib.lib user.lib -out:app.exe -lldjoint:facts/app.exe.jfacts

# RUN: llvm-readobj --coff-imports --sections lib.dll > lib.txt
# RUN: llvm-objdump -d --no-show-raw-insn lib.dll >> lib.txt
# RUN: FileCheck --check-prefix=LIB --input-file=lib.txt %s
# LIB:      Name: .sealed
# LIB-NOT:  Name: app.exe
# LIB:      <g>:
# LIB-NEXT: bl 0x[[#%x,THUNK:]]
# LIB-NEXT: adrp x0, 0x180005000
# LIB-NEXT: ldr x0, [x0]
# LIB:      [[#%x,THUNK]]: adrp x16, 0x180005000
# LIB-NEXT: ldr x16, [x16]
# LIB-NEXT: br x16

# RUN: llvm-readobj --coff-imports --sections user.dll | \
# RUN:   FileCheck --check-prefix=USER %s
# USER:     Name: .sealed
# USER-NOT: Name: app.exe
# USER-NOT: Name: lib.dll

# RUN: llvm-readobj --coff-exports app.exe | FileCheck --check-prefix=APP %s
# APP: Name: f

#--- lib.s
  .text
  .globl f
  .def f; .scl 2; .type 32; .endef
  .p2align 2
f:
  mov w0, #1
  ret
  .globl g
  .def g; .scl 2; .type 32; .endef
  .p2align 2
g:
  bl f
  adrp x0, f
  add x0, x0, :lo12:f
  ret
  .data
  .quad f
  .section .drectve,"yni"
  .ascii " -export:f -export:g"

#--- user.s
  .text
  .globl u
  .def u; .scl 2; .type 32; .endef
u:
  adrp x16, __imp_f
  ldr x16, [x16, :lo12:__imp_f]
  br x16
  .section .drectve,"yni"
  .ascii " -export:u"

#--- app.s
  .text
  .globl f
  .def f; .scl 2; .type 32; .endef
f:
  mov w0, #2
  ret
  .globl main
  .def main; .scl 2; .type 32; .endef
main:
  bl f
  adrp x16, __imp_u
  ldr x16, [x16, :lo12:__imp_u]
  br x16

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## A stand-in for the builtins' __llvm_fill.
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
