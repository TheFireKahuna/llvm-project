# REQUIRES: x86
## A function that an executable of a joint link defines, and that a member DLL
## whose import library its link line names exports too, takes the DLL's
## definition's place in every member's references, the DLL's own included, as
## an ELF executable's definition preempts a shared object's. The executable
## exports it, and the members import it from the executable that loads them:
## the DLL's branches to its definition reach the import's thunk, its lea loads
## the import, and its data word is written from the import. The DLL's export
## stays its own definition. An executable of the set that loads the DLL but
## does not define the function leaves the DLL its own, with a warning; so does
## -Bsymbolic for the DLL's own references alone. Outside a joint link, the
## executable's definition is reported where the DLL links under -import-slots.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium user.s -o user.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tool.s -o tool.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc lib.s -o msvc.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -def:other.def -out:other.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry lib.obj crt.obj ntdll.lib \
# RUN:   -out:lib.dll -lldjointscan:lib.jscan
# RUN: lld-link -import-slots -dll -noentry user.obj crt.obj lib.lib \
# RUN:   ntdll.lib -out:user.dll -lldjointscan:user.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj lib.lib \
# RUN:   user.lib -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console tool.obj \
# RUN:   user.lib -out:tool.exe -lldjointscan:tool.jscan
# RUN: lld-link -lldjointmerge -out:facts lib.jscan user.jscan app.jscan

# RUN: lld-link -import-slots -dll -noentry lib.obj crt.obj ntdll.lib \
# RUN:   -out:lib.dll -lldjoint:facts/lib.dll.jfacts -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=LOG %s
# RUN: lld-link -import-slots -dll -noentry user.obj crt.obj lib.lib \
# RUN:   ntdll.lib -out:user.dll -lldjoint:facts/user.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj lib.lib \
# RUN:   user.lib -out:app.exe -lldjoint:facts/app.exe.jfacts 2>&1 | \
# RUN:   count 0
# LOG: Keeping __imp_f for f, which the image gives up

## lib.dll keeps exporting its own f and g, imports f, and app.exe's identity,
## from no descriptor, and reaches f through its cell in .sealed: g's call
## through the import's thunk, its lea as a load, its data word from the fill's
## list.
# RUN: llvm-readobj --coff-imports --coff-exports --sections lib.dll > lib.txt
# RUN: llvm-objdump -d lib.dll >> lib.txt
# RUN: llvm-objdump -s -j .rdata lib.dll >> lib.txt
# RUN: FileCheck --check-prefix=LIB --input-file=lib.txt %s
# LIB:      Name: .sealed
# LIB-NEXT: VirtualSize: 0x10
# LIB-NEXT: VirtualAddress: 0x5000
# LIB-NOT:  Name: app.exe
# LIB:      Name: f
# LIB-NEXT: RVA: 0x1000
# LIB:      Name: g
# LIB-NEXT: RVA: 0x1010
# LIB:      <f>:
# LIB:      <g>:
# LIB-NEXT: callq 0x[[#%x,THUNK:]]
# LIB-NEXT: movq {{.*}}(%rip), %rax # 0x180005000
# LIB:      [[#%x,THUNK]]: ff 25 {{.*}} jmpq *{{.*}}(%rip) # 0x180005000
## The residual word {.data, cell, addend} and the cells {cell, name, from the
## executable} of f and of the identity, after the list's 40-byte header.
# LIB:      180002020 01000000 00000000 00300000 00500000
# LIB-NEXT: 180002030 00000000 00000000 00500000 {{[0-9a-f]+}}
# LIB-NEXT: 180002040 00000000 08500000 {{[0-9a-f]+}} 00000000
# LIB-NEXT: 180002050 66005f5f 6c6c766d 5f707269 76617465

## user.dll imports f from the executable and g from lib.dll, with the
## identities of both.
# RUN: llvm-readobj --coff-imports --sections user.dll | \
# RUN:   FileCheck --check-prefix=USER %s
# USER:      Name: .sealed
# USER-NOT:  Name: app.exe
# USER:      Name: lib.dll
# USER-NEXT: ImportLookupTableRVA:
# USER-NEXT: ImportAddressTableRVA:
# USER-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# USER-NEXT: Symbol: g (0)
# USER-NEXT: }
# USER-NOT:  Name: app.exe

## app.exe exports f for them.
# RUN: llvm-readobj --coff-exports app.exe | FileCheck --check-prefix=APP %s
# APP: Name: f

## tool.exe loads lib.dll through user.dll but does not define f.
# RUN: lld-link -lldjointmerge -out:facts2 lib.jscan user.jscan app.jscan \
# RUN:   tool.jscan 2>&1 | FileCheck --check-prefix=DISAGREE %s
# DISAGREE: warning: app.exe defines f, which lib.dll exports, but tool.exe, which loads lib.dll too, does not; every member keeps binding f to lib.dll's
# RUN: mkdir own bsym msvc
# RUN: lld-link -import-slots -dll -noentry lib.obj crt.obj ntdll.lib \
# RUN:   -out:own/lib.dll -lldjoint:facts2/lib.dll.jfacts
# RUN: llvm-objdump -d own/lib.dll | FileCheck --check-prefix=OWN %s
# OWN:      <g>:
# OWN-NEXT: callq {{.*}} <f>
# OWN-NEXT: leaq {{.*}} <f>

## -Bsymbolic-functions keeps lib.dll's own references to its definitions.
# RUN: lld-link -import-slots -Bsymbolic-functions -dll -noentry lib.obj \
# RUN:   crt.obj ntdll.lib -out:lib.dll -lldjointscan:blib.jscan
# RUN: lld-link -import-slots -Bsymbolic-functions -dll -noentry user.obj \
# RUN:   crt.obj lib.lib ntdll.lib -out:user.dll -lldjointscan:buser.jscan
# RUN: lld-link -import-slots -Bsymbolic-functions -entry:main \
# RUN:   -subsystem:console app.obj lib.lib user.lib -out:app.exe \
# RUN:   -lldjointscan:bapp.jscan
# RUN: lld-link -lldjointmerge -out:bfacts blib.jscan buser.jscan bapp.jscan
# RUN: lld-link -import-slots -Bsymbolic-functions -dll -noentry lib.obj \
# RUN:   crt.obj ntdll.lib -out:bsym/lib.dll -lldjoint:bfacts/lib.dll.jfacts
# RUN: llvm-objdump -d bsym/lib.dll | FileCheck --check-prefix=OWN %s

## An object that does not describe its references to the function cannot have
## them reach the executable's definition.
# RUN: not lld-link -import-slots -dll -noentry msvc.obj crt.obj ntdll.lib \
# RUN:   -out:msvc/lib.dll -lldjoint:facts/lib.dll.jfacts 2>&1 | \
# RUN:   FileCheck --check-prefix=ERR %s
# ERR: error: msvc.obj: f is defined by the executable of its joint link, but its object does not describe its references to it, which therefore cannot reach the program's choice

## Outside a joint link, the executable's f is reported for lib.dll, whose
## import library records it links under -import-slots, but not for a DLL
## whose library does not.
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj lib.lib \
# RUN:   user.lib -out:plain.exe 2>&1 | FileCheck --check-prefix=WARN %s
# WARN: warning: app.obj: f is also exported by lib.dll, whose own references keep its definition; link them jointly for this one to take its place
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   other.lib user.lib -out:plain2.exe 2>&1 | count 0

#--- lib.s
        .text
        .globl f
        .def f; .scl 2; .type 32; .endef
        .p2align 4
f:
        movl $1, %eax
        retq
        .globl g
        .def g; .scl 2; .type 32; .endef
        .p2align 4
g:
        callq f
        leaq f(%rip), %rax
        retq
        .data
        .quad f
        .section .drectve,"yni"
        .ascii " -export:f -export:g"

#--- user.s
        .text
        .globl u
        .def u; .scl 2; .type 32; .endef
        .p2align 4
u:
        callq *__imp_f(%rip)
        callq *__imp_g(%rip)
        retq
        .section .drectve,"yni"
        .ascii " -export:u"

#--- app.s
        .text
        .globl f
        .def f; .scl 2; .type 32; .endef
        .p2align 4
f:
        movl $2, %eax
        retq
        .globl main
        .def main; .scl 2; .type 32; .endef
        .p2align 4
main:
        callq f
        callq *__imp_u(%rip)
        retq

#--- tool.s
        .text
        .globl main
        .def main; .scl 2; .type 32; .endef
main:
        callq *__imp_u(%rip)
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

## A stand-in for the builtins' __llvm_fill.
        .def __llvm_fill; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,__llvm_fill
        .globl __llvm_fill
__llvm_fill:
        leaq __llvm_fill_list(%rip), %rax
        callq *__imp_NtProtectVirtualMemory(%rip)
        callq *__imp_LdrGetProcedureAddressForCaller(%rip)
        retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

#--- other.def
LIBRARY other.dll
EXPORTS
  f
