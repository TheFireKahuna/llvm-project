# REQUIRES: x86
## Under -import-slots, a DLL that offers an overridable default reaches its own
## definition as every other image does, through the default's import: the
## program's choice, which the loader binds and the DLL's fill writes from the
## executable's table. A definition the program does not select is preempted
## in the DLL itself, as ELF preempts a shared object's definition. The DLL's
## calls of the definition reach the import's thunk, also from the unit that
## defines it, whose object keeps every relocation to a function and describes
## the sites that are not branches; its address-takes, a described lea, load
## the import's entry; a word of its static data naming the definition becomes
## an in-place slot of the import; and the DLL still exports its body under the
## offer name. Without the runtime's import library, or for a definition it
## does not export, the DLL keeps its definition to itself, and so does the
## executable.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium offer.s -o offer.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium hidden.s -o hidden.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium check.s -o check.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib

# RUN: lld-link -import-slots -dll -noentry offer.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:offer.dll -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=LOG %s
# RUN: llvm-readobj --coff-exports --coff-imports offer.dll > offer.txt
# RUN: llvm-objdump -d offer.dll >> offer.txt
# RUN: llvm-objdump -s -j .rdata offer.dll >> offer.txt
# RUN: FileCheck --check-prefix=OFFER --input-file=offer.txt %s
# LOG: Keeping __imp__Znwy for operator new(unsigned long long), which the image offers

## The import's entry, which the calls and the address-take read, and the
## in-place slot of the data word are both cells of the default, in the fill's
## list.
# OFFER:      Name: rt.dll
# OFFER-NEXT: ImportLookupTableRVA:
# OFFER-NEXT: ImportAddressTableRVA: 0x[[#%X,ENTRY:]]
# OFFER-NEXT: Symbol: _Znwy
# OFFER:      Name: rt.dll
# OFFER-NEXT: ImportLookupTableRVA:
# OFFER-NEXT: ImportAddressTableRVA: 0x[[#%X,SLOT:]]
# OFFER-NEXT: Symbol: _Znwy
# OFFER:      Name: __ovr_offer$_Znwy
# OFFER-NEXT: RVA: 0x[[#%X,BODY:]]
# OFFER:      <.text>:
# OFFER-NEXT: e8 {{.*}} callq 0x[[#%x,THUNK:]]
# OFFER-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,0x180000000+ENTRY]]
# OFFER-NEXT: c3 retq
# OFFER:      [[#%x,0x180000000+BODY]] <__ovr_offer$_Znwy>:
# OFFER:      leaq {{.*}}(%rip), %rbx # 0x[[#%x,LIST:]]
# OFFER:      [[#THUNK]]: ff 25 {{.*}} jmpq *{{.*}}(%rip) # 0x[[#%x,0x180000000+ENTRY]]
# OFFER:      [[#%x,LIST]] 00000000 44000000 00300000 02000000
# OFFER-NEXT: [[#%x,LIST+0x10]] 01000000 44000000 e0200000 02000000

## Without the runtime's import library there is no import to reach, and the
## definition stays the DLL's own.
# RUN: lld-link -import-slots -dll -noentry offer.obj crt.obj -out:own.dll
# RUN: llvm-objdump -d own.dll | FileCheck --check-prefix=OWN %s
# OWN:      <.text>:
# OWN-NEXT: e8 {{.*}} callq 0x[[#%x,OWNBODY:]]
# OWN-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#OWNBODY]]

## A definition the DLL does not export makes no offer, and is its own.
# RUN: lld-link -import-slots -dll -noentry hidden.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:hidden.dll
# RUN: llvm-objdump -d hidden.dll | FileCheck --check-prefix=OWN %s

## The executable's replacement is the program's.
# RUN: lld-link -import-slots -entry:dllmain -subsystem:console hidden.obj \
# RUN:   crt.obj check.obj rt.lib ntdll.lib -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck --check-prefix=OWN %s

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .quad 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .quad 0

#--- check.s
## The executable's start-up validation, which the C++ runtime provides.
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0

#--- offer.s
        .text
        .globl dllmain
dllmain:
        callq _Znwy
        leaq _Znwy(%rip), %rax
        retq
        .def _Znwy; .scl 2; .type 32; .endef
        .globl _Znwy
_Znwy:
        xorl %eax, %eax
        retq
        .data
        .globl gnew
gnew:
        .quad _Znwy
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy /INCLUDE:gnew"

#--- hidden.s
        .text
        .globl dllmain
dllmain:
        callq _Znwy
        leaq _Znwy(%rip), %rax
        retq
        .def _Znwy; .scl 2; .type 32; .endef
        .globl _Znwy
_Znwy:
        xorl %eax, %eax
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy"
