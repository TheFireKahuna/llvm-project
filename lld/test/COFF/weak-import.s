# REQUIRES: x86

## A compiler reaches an extern_weak X through __imp_X and keeps X a weak
## external of the object. Under -import-slots an import library's offer
## satisfies the reference, as a shared object satisfies an ELF weak
## reference, and is bound as ELF binds a weak undefined symbol, when the image
## starts: __imp_X is a cell in .sealed, which the image's fill writes with
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
# RUN: llvm-objdump -s -j .rdata a.exe >> a.txt
# RUN: FileCheck --input-file=a.txt %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x6000
# CHECK-NOT:  Name: dll.dll
# CHECK:      Name: ntdll.dll
# CHECK:      Symbol: LdrGetDllHandleEx
# CHECK-NEXT: Symbol: LdrGetProcedureAddressForCaller
# CHECK-NEXT: Symbol: NtProtectVirtualMemory
# CHECK-NOT:  Name: dll.dll

## f and v read their cells; the archive's g is not loaded, so g is zero, as
## absent is.
# CHECK:      <.text>:
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x140006000
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x140006000
# CHECK-NEXT: movq {{.*}}(%rip), %rcx # 0x140006008
# CHECK-NEXT: movq $0x0, %rdx

## For each import the fill finds dll.dll among the loaded modules, its name a
## UNICODE_STRING, leaving the cell null if it is not there, and then the
## import in it, with flag 1, which binds without initializing the DLL. It
## then writes the word of .data from v's cell, and seals .sealed.
# CHECK:      leaq {{.*}}(%rip), %rbx # 0x[[#%x,CELLS:]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rsi # 0x[[#%x,CELLS+0x20]]
# CHECK-NEXT: cmpq %rsi, %rbx
# CHECK-NEXT: jae
# CHECK-NEXT: movl 0x8(%rbx), %eax
# CHECK-NEXT: cmpl $0x1, %eax
# CHECK-NEXT: jbe 0x[[#%x,HOST:]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rcx # 0x140000000
# CHECK-NEXT: addq %rcx, %rax
# CHECK-NEXT: movq %rax, 0x50(%rsp)
# CHECK-NEXT: movzwl 0xe(%rbx), %eax
# CHECK-NEXT: leal 0x2(%rax), %ecx
# CHECK-NEXT: shll $0x10, %ecx
# CHECK-NEXT: orl %ecx, %eax
# CHECK-NEXT: movl %eax, 0x48(%rsp)
# CHECK-NEXT: movl $0x1, %ecx
# CHECK-NEXT: xorl %edx, %edx
# CHECK-NEXT: xorl %r8d, %r8d
# CHECK-NEXT: leaq 0x48(%rsp), %r9
# CHECK-NEXT: leaq 0x58(%rsp), %rax
# CHECK-NEXT: movq %rax, 0x20(%rsp)
# CHECK-NEXT: callq *{{.*}}(%rip)
# CHECK-NEXT: testl %eax, %eax
# CHECK-NEXT: js 0x[[#%x,NEXT:]]
# CHECK-NEXT: movq 0x58(%rsp), %rcx
# CHECK-NEXT: jmp
# CHECK-NEXT: [[#%x,HOST]]: {{.*}} movq %gs:0x60, %rcx
# CHECK:      cmpl $0x0, 0x8(%rbx)
# CHECK-NEXT: jne 0x[[#%x,NEXT]]
# CHECK:      leaq {{.*}}(%rip), %rbx # 0x[[#%x,CELLS+0x20]]
# CHECK:      movl $0x2, %r9d

## The imports, {cell, name, the DLL's name, the names' lengths}: f and v,
## each from dll.dll; then the word, {word, cell, 0}.
# CHECK:      [[#%x,CELLS]] 00600000 30200000 32200000 01000e00
# CHECK-NEXT: [[#%x,CELLS+0x10]] 08600000 42200000 44200000 01000e00
# CHECK-NEXT: [[#%x,CELLS+0x20]] 00300000 08600000 00000000 00000000
# CHECK-NEXT: 66006400 6c006c00 2e006400 6c006c00  f.d.l.l...d.l.l.

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
