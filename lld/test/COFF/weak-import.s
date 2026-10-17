# REQUIRES: x86

## A compiler reaches an extern_weak X through __imp_X and keeps X a weak
## external of the object. Under -import-slots an import library's offer
## satisfies the reference, as a shared object satisfies an ELF weak
## reference, and is bound as ELF binds a weak undefined symbol, when the image
## starts: __imp_X is a cell in .sealed, which the residual fill writes with
## the export's address if the DLL is loaded and has it, and leaves null
## otherwise, and a word of static data holding X's address is written from
## the cell. A DLL whose only imports are weak is not a dependency. A
## reference other than a weak one keeps the import the loader's. An archive
## member defining X does not satisfy the reference, since a weak external
## loads no archive member, and an absent X reads zero.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium strong.s -o strong.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-lib -out:lib.lib lib.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -dll -noentry -out:dll.dll -implib:dll.lib dll.obj \
# RUN:   -export:f -export:v,DATA

# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj crt.obj dll.lib lib.lib ntdll.lib 2>&1 | count 0
# RUN: llvm-readobj --coff-imports --sections a.exe > a.txt
# RUN: llvm-objdump -d --print-imm-hex a.exe >> a.txt
# RUN: FileCheck --input-file=a.txt %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x[[#%x,SEALED:]]
# CHECK-NOT:  Name: dll.dll
# CHECK:      Name: ntdll.dll
# CHECK:      Symbol: LdrGetDllHandleEx
# CHECK-NEXT: Symbol: LdrGetProcedureAddressForCaller
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK-NOT:  Name: dll.dll

## f and v read their cells; the archive's g is not loaded, so g is zero, as
## absent is.
# CHECK:      <.text>:
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#SEALED + 0x140000000]]
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#SEALED + 0x140000000]]
# CHECK-NEXT: movq {{.*}}(%rip), %rcx # 0x[[#SEALED + 0x140000008]]
# CHECK-NEXT: movq $0x0, %rdx

## The fill finds dll.dll among the loaded modules, leaving f's cell null if it
## is not there, and then f in it, with flag 1, which binds without
## initializing the DLL; and v. It then writes the word of .data from v's
## cell, and seals .sealed.
# CHECK:      subq $0x48, %rsp
# CHECK:      movl $0x1, %ecx
# CHECK:      callq *{{.*}}(%rip)
# CHECK-NEXT: testl %eax, %eax
# CHECK-NEXT: js
# CHECK-NEXT: movq 0x40(%rsp), %rcx
# CHECK:      leaq {{.*}}(%rip), %r9 # 0x[[#SEALED + 0x140000000]]
# CHECK-NEXT: movl $0x1, 0x20(%rsp)
# CHECK:      leaq {{.*}}(%rip), %r9 # 0x[[#SEALED + 0x140000008]]
# CHECK:      movq {{.*}}(%rip), %rax # 0x[[#SEALED + 0x140000008]]
# CHECK-NEXT: addq $0x0, %rax
# CHECK-NEXT: movq %rax, {{.*}}(%rip)
# CHECK:      movl $0x2, %r9d
# CHECK-NEXT: callq *{{.*}}(%rip)
# CHECK-NEXT: addq $0x48, %rsp
# CHECK-NEXT: retq

## A reference other than a weak one, as a declaration marked dllimport gives,
## keeps f's import the loader's.
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:b.exe \
# RUN:   main.obj strong.obj crt.obj dll.lib lib.lib ntdll.lib
# RUN: llvm-readobj --coff-imports b.exe | FileCheck --check-prefix=STRONG %s
# STRONG:      Name: dll.dll
# STRONG-NEXT: ImportLookupTableRVA:
# STRONG-NEXT: ImportAddressTableRVA:
# STRONG-NEXT: Symbol: f

## Without -import-slots the import is the loader's, and no member defining g
## is loaded either.
# RUN: lld-link -entry:main -subsystem:console -out:c.exe main.obj dll.lib \
# RUN:   lib.lib
# RUN: llvm-objdump -d c.exe | FileCheck --check-prefix=NOSLOTS %s
# NOSLOTS: 48 c7 c2 00 00 00 00 movq $0x0, %rdx

## Only under -import-slots is a reference that reads the pointer to an absent
## weak symbol's zero kept out of the locally-defined-import warning.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc absent.s -o absent.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:d.exe \
# RUN:   absent.obj 2>&1 | count 0
# RUN: lld-link -entry:main -subsystem:console -out:e.exe absent.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s

# WARN: warning: absent.obj: locally defined symbol imported: .weak.absent.default.main (defined in <internal>) [LNK4217]

#--- main.s
  .text
  .globl main
main:
  movq __imp_f(%rip), %rax
  callq *__imp_f(%rip)
  movq __imp_v(%rip), %rcx
  movq __imp_g(%rip), %rdx
  cmpq $0, __imp_absent(%rip)
  retq

  .data
  .quad v

  .weak f
  .weak v
  .weak g
  .weak absent

#--- strong.s
  .text
  .globl strong
strong:
  callq *__imp_f(%rip)
  retq

#--- dll.s
  .text
  .globl f
f:
  retq

  .data
  .globl v
v:
  .long 1

#--- lib.s
  .text
  .globl g
g:
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

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrGetDllHandleEx
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

#--- absent.s
  .text
  .globl main
main:
  cmpq $0, __imp_absent(%rip)
  retq

  .weak absent
