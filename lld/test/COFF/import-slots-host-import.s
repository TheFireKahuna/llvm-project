# REQUIRES: x86
## Under -import-slots, an import of a DLL from an executable can only mean the
## process's own executable, whatever its file name, so it is bound by name
## when the DLL starts rather than by a descriptor naming the file: its pointer
## is a cell in .sealed, which the image's fill, the builtins' __llvm_fill,
## writes at run time against PEB->ImageBaseAddress, and a word of static data
## holding the address is written from the cell. The run-time cells and residual
## word sit in a list the linker builds. -unresolved-host-imports imports what
## nothing defines from the executable, as code where an object declares it a
## function and as data otherwise. Without -import-slots the import is the
## loader's.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plug.s -o plug.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:host.def -out:host.lib -machine:x64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry plug.obj crt.obj host.lib \
# RUN:   ntdll.lib -out:plug.dll
# RUN: llvm-readobj --coff-imports --sections plug.dll > plug.txt
# RUN: llvm-objdump -d --print-imm-hex plug.dll >> plug.txt
# RUN: llvm-objdump -s -j .rdata plug.dll >> plug.txt
# RUN: FileCheck --input-file=plug.txt %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x5000
# CHECK-NOT:  Name: host.exe
# CHECK:      Name: ntdll.dll
# CHECK:      Symbol: LdrGetProcedureAddressForCaller
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK-NOT:  Name: host.exe

## hostfn and hostvar read their cells in .sealed.
# CHECK:      <.text>:
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x180005000
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x180005008

## __llvm_fill loads its list.
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]

## The list: no cells, two run-time cells, one residual word; the section it
## seals is .sealed at 0x5000, size 0x10; its flag word's bit 0 set, since the
## DLL imports from the executable and runs its copy.
# CHECK:      [[#%x,LIST]]      00000000 00000000 02000000 01000000
# CHECK-NEXT: [[#%x,LIST+0x10]] 00000000 00000000 00500000 10000000
# CHECK-NEXT: [[#%x,LIST+0x20]] {{[0-9a-f]+ [0-9a-f]+}} 01000000 00000000
## The run-time cells, {cell, name, 0 for one from the executable that is not
## weak, the name's length}: hostfn and hostvar; then the residual word {word,
## cell, addend} for the .data copy.
# CHECK-NEXT: [[#%x,LIST+0x30]] 00500000 60200000 00000000 06000000
# CHECK-NEXT: [[#%x,LIST+0x40]] 08500000 67200000 00000000 07000000
# CHECK-NEXT: [[#%x,LIST+0x50]] 00300000 00500000 00000000 00000000
# CHECK-NEXT: [[#%x,LIST+0x60]] 686f7374 666e0068 6f737476 61720000  hostfn.hostvar..

## The names are imported from the executable that loads the DLL.
# RUN: lld-link -import-slots -dll -noentry plug.obj crt.obj ntdll.lib \
# RUN:   -out:unresolved.dll -unresolved-host-imports -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=UNRESOLVED %s
# UNRESOLVED: Importing hostfn from the executable that loads the image
# UNRESOLVED: Importing hostvar from the executable that loads the image
# RUN: llvm-readobj --coff-imports unresolved.dll | \
# RUN:   FileCheck --check-prefix=NODESC %s
# NODESC-NOT: Name: <host>.exe

## Without it, they are undefined.
# RUN: not lld-link -import-slots -dll -noentry plug.obj crt.obj ntdll.lib \
# RUN:   -out:undefined.dll 2>&1 | FileCheck --check-prefix=UNDEFINED %s
# UNDEFINED: undefined symbol: __declspec(dllimport) hostfn

## Without -import-slots the import is the loader's, through a descriptor
## naming host.exe.
# RUN: lld-link -dll -noentry plug.obj host.lib -out:loader.dll
# RUN: llvm-readobj --coff-imports loader.dll | FileCheck --check-prefix=LOADER %s
# LOADER: Name: host.exe

#--- plug.s
        .def hostfn
        .scl 2
        .type 32
        .endef
        .text
        .globl dllmain
dllmain:
        callq *__imp_hostfn(%rip)
        movq __imp_hostvar(%rip), %rax
        retq
        .data
        .quad hostfn

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
## the ntdll functions it binds run-time host imports and seals with.
        .def __llvm_fill
        .scl 2
        .type 32
        .endef
        .section .text,"xr",one_only,__llvm_fill
        .globl __llvm_fill
__llvm_fill:
        leaq __llvm_fill_list(%rip), %rax
        callq *__imp_NtProtectVirtualMemory(%rip)
        callq *__imp_LdrGetProcedureAddressForCaller(%rip)
        retq

#--- host.def
LIBRARY host.exe
EXPORTS
  hostfn
  hostvar DATA

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
