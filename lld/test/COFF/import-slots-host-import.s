# REQUIRES: x86
## Under -import-slots, an import of a DLL from an executable can only mean the
## process's own executable, whatever its file name, so it is bound by name
## when the DLL starts rather than by a descriptor naming the file: its pointer
## is a cell in .sealed, which the residual fill writes through
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
# RUN: FileCheck --input-file=plug.txt %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x[[#%x,SEALED:]]
# CHECK-NOT:  Name: host.exe
# CHECK:      Name: ntdll.dll
# CHECK:      Symbol: LdrGetProcedureAddressForCaller
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK-NOT:  Name: host.exe

# CHECK:      <.text>:
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#SEALED + 0x180000000]]
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#SEALED + 0x180000008]]

## Each name is resolved in the executable, the PEB's ImageBaseAddress, with
## flag 1, which binds without initializing it; a failure ends the fill with
## its status.
# CHECK:      subq $0x48, %rsp
# CHECK-NEXT: movq %gs:0x60, %rcx
# CHECK-NEXT: movq 0x10(%rcx), %rcx
# CHECK-NEXT: leaq {{.*}}(%rip), %rax
# CHECK-NEXT: movq %rax, 0x38(%rsp)
# CHECK-NEXT: movl $0x70006, 0x30(%rsp)
# CHECK-NEXT: leaq 0x30(%rsp), %rdx
# CHECK-NEXT: xorl %r8d, %r8d
# CHECK-NEXT: leaq {{.*}}(%rip), %r9 # 0x[[#SEALED + 0x180000000]]
# CHECK-NEXT: movl $0x1, 0x20(%rsp)
# CHECK-NEXT: leaq {{.*}}(%rip), %rax
# CHECK-NEXT: movq %rax, 0x28(%rsp)
# CHECK-NEXT: callq *{{.*}}(%rip)
# CHECK-NEXT: testl %eax, %eax
# CHECK-NEXT: js 0x[[#%x,END:]]
# CHECK:      leaq {{.*}}(%rip), %r9 # 0x[[#SEALED + 0x180000008]]
# CHECK:      js 0x[[#END]]
## The word of .data holding hostfn's address, from its cell; then the seal.
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#SEALED + 0x180000000]]
# CHECK-NEXT: addq $0x0, %rax
# CHECK-NEXT: movq %rax, {{.*}}(%rip)
# CHECK:      movl $0x2, %r9d
# CHECK-NEXT: callq *{{.*}}(%rip)
# CHECK-NEXT: [[#END]]: {{.*}} addq $0x48, %rsp
# CHECK-NEXT: retq

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
