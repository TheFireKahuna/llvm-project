# REQUIRES: x86
## Under -import-slots, an import of a DLL from an executable can only mean the
## process's own executable, whatever its file name, so it is bound by name
## when the DLL starts rather than by a descriptor naming the file: its pointer
## is a cell in .sealed, which the image's fill writes through
## LdrGetProcedureAddressForCaller against PEB->ImageBaseAddress, failing the
## DLL's initialization with the resolver's status when the executable lacks
## the name, and a word of static data holding the address is written from the
## cell. -unresolved-host-imports imports what nothing defines from the
## executable, as code where an object declares it a function and as data
## otherwise. Without -import-slots the import is the loader's.

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
# CHECK-NEXT: VirtualAddress: 0x6000
# CHECK-NOT:  Name: host.exe
# CHECK:      Name: ntdll.dll
# CHECK:      Symbol: LdrGetProcedureAddressForCaller
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK-NOT:  Name: host.exe

# CHECK:      <.text>:
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x180006000
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x180006008

## Each name of the list's imports is resolved in the executable, the PEB's
## ImageBaseAddress, with flag 1, which binds without initializing it; a
## failure ends the fill with its status.
# CHECK:      leaq {{.*}}(%rip), %rbx # 0x[[#%x,CELLS:]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rsi # 0x[[#%x,CELLS+0x20]]
# CHECK-NEXT: cmpq %rsi, %rbx
# CHECK-NEXT: jae
# CHECK-NEXT: movq %gs:0x60, %rcx
# CHECK-NEXT: movq 0x10(%rcx), %rcx
# CHECK-NEXT: movl 0x4(%rbx), %eax
# CHECK-NEXT: leaq {{.*}}(%rip), %rdx # 0x180000000
# CHECK-NEXT: addq %rdx, %rax
# CHECK-NEXT: movq %rax, 0x50(%rsp)
# CHECK-NEXT: movzwl 0xc(%rbx), %eax
# CHECK-NEXT: leal 0x1(%rax), %edx
# CHECK-NEXT: shll $0x10, %edx
# CHECK-NEXT: orl %edx, %eax
# CHECK-NEXT: movl %eax, 0x48(%rsp)
# CHECK-NEXT: leaq 0x48(%rsp), %rdx
# CHECK-NEXT: xorl %r8d, %r8d
# CHECK-NEXT: movl (%rbx), %r9d
# CHECK-NEXT: leaq {{.*}}(%rip), %rax # 0x180000000
# CHECK-NEXT: addq %rax, %r9
# CHECK-NEXT: movl $0x1, 0x20(%rsp)
# CHECK-NEXT: leaq {{.*}}(%rip), %rax # 0x180001010
# CHECK-NEXT: movq %rax, 0x28(%rsp)
# CHECK-NEXT: callq *{{.*}}(%rip)
# CHECK-NEXT: cmpl $0x0, 0x8(%rbx)
# CHECK-NEXT: jne
# CHECK-NEXT: testl %eax, %eax
# CHECK-NEXT: js 0x[[#%x,FAIL:]]
## Then the word of .data holding hostfn's address, from its cell, and the
## seal.
# CHECK:      leaq {{.*}}(%rip), %rbx # 0x[[#%x,CELLS+0x20]]
# CHECK:      [[#%x,FAIL]]: {{.*}} movl %eax, %r12d
# CHECK:      movl $0x2, %r9d

## The imports, {cell, name, 0 for one from the executable that is not weak,
## the name's length}: hostfn and hostvar; then the word, {word, cell, 0}.
# CHECK:      [[#%x,CELLS]] 00600000 30200000 00000000 06000000
# CHECK-NEXT: [[#%x,CELLS+0x10]] 08600000 37200000 00000000 07000000
# CHECK-NEXT: [[#%x,CELLS+0x20]] 00300000 00600000 00000000 00000000
# CHECK-NEXT: 686f7374 666e0068 6f737476 61720000  hostfn.hostvar..

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

# RUN: lld-link -dll -noentry plug.obj crt.obj host.lib -out:loader.dll
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
