# REQUIRES: x86
## Under -import-slots, a DLL loaded outside the executable's link binds the
## cells of its vague entities as an ELF object opened RTLD_LOCAL resolves
## against the global scope. A DLL that keeps an entity preemptible, or that
## delay-loads a DLL, names a table in its MZ header as {tag, rva} at 0x30:
## the shift and mask of its buckets, the number and offset of the keys of the
## DLLs it delay-loads, then the buckets, each naming the DLL's pointer to an
## entity, which holds the instance it uses, then those keys. A DLL with
## cells, of its own entities or of an entity that a DLL it imports from keeps
## preemptible, runs a search from its C initializer table: once the process
## has initialized, for each entity the executable's table does not name, it
## probes the table of each module loaded with the process, or that
## delay-loads it, writes the instance into the entity's cells, and pins the
## module and itself.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x.s -o x.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -def:protect.def -out:protect.lib -machine:x64
# RUN: lld-link -dll -noentry x.obj -out:x.dll

# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj x.lib ntdll.lib \
# RUN:   -delayload:x.dll -out:a.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 a.dll > a.txt
# RUN: llvm-objdump -s -j .rdata a.dll >> a.txt
# RUN: llvm-readobj --coff-imports a.dll >> a.txt
# RUN: llvm-objdump -d --print-imm-hex a.dll >> a.txt
# RUN: FileCheck --input-file=a.txt %s

## The table: two buckets, one delay-loaded DLL's key at 0x30; the bucket of
## _ZN1n1gE's key names its pointer, and x.dll's key follows the buckets.
# CHECK:      000030 6c8e9cf5 [[#%x,TABLE:]]
# CHECK:      [[#TABLE + 0x180000000]] 3f000000 01000000 01000000 30000000
# CHECK-NEXT: b65fec27 d80a3472 {{[0-9a-f]+}} 00000000
# CHECK-NEXT: 00000000 00000000 00000000 00000000
# CHECK-NEXT: 5ac27ce3 f11f7492

# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,NTDLL:]]
# CHECK-NEXT: Symbol: LdrAddRefDll
# CHECK-NEXT: Symbol: NtProtectVirtualMemory

## Bit 2 of r14 records a DLL loaded after the process initialized. For an
## entity the executable's table does not name, such a DLL searches the
## modules after the executable, each with the table's tag inside its image,
## probed if loaded with the process, if delay-loaded within that scope, or if
## it delay-loads this DLL, by the key of this DLL's name.
# CHECK:      pushq %rbx
# CHECK:      xorl %r14d, %r14d
# CHECK-NEXT: movq %gs:0x60, %rax
# CHECK-NEXT: testb $0x2, 0x50(%rax)
# CHECK-NEXT: jne
# CHECK-NEXT: orl $0x4, %r14d
# CHECK-NEXT: movq 0x10(%rax), %rdi
# CHECK-NEXT: cmpl $0x7b3ac2d5, 0x80(%rdi)
# CHECK:      testb $0x2, 0xc(%rbx)
# CHECK-NEXT: je
# CHECK-NEXT: btl $0x2, %r14d
# CHECK-NEXT: jae
# CHECK:      movq 0x18(%rax), %rbp
# CHECK-NEXT: addq $0x10, %rbp
# CHECK:      movq 0x30(%r15), %rdx
# CHECK:      cmpl $0x6c8e9cf5, 0x30(%rdx)
# CHECK:      movl 0x34(%rdx), %r8d
# CHECK-NEXT: cmpl 0x40(%r15), %r8d
# CHECK:      testb $0x20, 0x68(%r15)
## A delay-loaded module counts if its importer, by the base the loader
## records, is the executable or loaded with the process, in turn.
# CHECK:      cmpl $0x3, 0x10c(%rax)
# CHECK:      movq 0xb8(%rax), %r9
# CHECK:      cmpq %r9, 0x30(%rax)
# CHECK:      cmpq (%rbp), %rax
# CHECK:      testb $0x20, 0x68(%rax)
# CHECK:      movabsq $0x58a7bdf45370db42, %rax
## On a hit, LdrAddRefDll(LDR_ADDREF_DLL_PIN, module), then once this DLL; the
## pointer is written between two protection calls.
# CHECK:      movq (%rdx,%rax), %r13
# CHECK:      movl $0x1, %ecx
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,NTDLL + 0x180000000]]
# CHECK:      movl $0x1, %ecx
# CHECK-NEXT: leaq {{.*}}(%rip), %rdx # 0x180000000
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,NTDLL + 0x180000000]]
# CHECK:      movl $0x4, %r9d
# CHECK:      movq %r13, (%rcx,%rax)
## An entity the table names takes the word its entry names, then the
## instance from it; such a DLL pins itself.
# CHECK:      movq 0x8(%rax), %r13
# CHECK-NEXT: testb $0x2, 0xc(%rbx)
# CHECK-NEXT: je
# CHECK-NEXT: movq (%r13), %r13
# CHECK-NEXT: btl $0x2, %r14d
# CHECK-NEXT: jae
# CHECK-NEXT: btsl $0x1, %r14d
# CHECK:      popq %rbx
# CHECK-NEXT: retq

## The fill's list holds the pointer, read-only and a vague entity's.
# RUN: llvm-objdump -s -j .rdata a.dll | FileCheck --check-prefix=BIND %s
# BIND: b65fec27 d80a3472 {{[0-9a-f]+}} 03000000

## An importer of an entity that its DLL keeps preemptible reaches it through
## a cell, the import's entry, which the fill writes from the executable's
## table and the search from a loaded module's; it keeps no entity itself and
## delay-loads nothing, so it has no table.
# RUN: lld-link -import-slots -dll -noentry c.obj crt.obj a.lib ntdll.lib \
# RUN:   -out:c.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 c.dll > c.txt
# RUN: llvm-readobj --coff-imports c.dll >> c.txt
# RUN: llvm-objdump -d --print-imm-hex c.dll >> c.txt
# RUN: FileCheck --check-prefix=IMPORTER --input-file=c.txt %s
# IMPORTER:      000030 00000000 00000000
# IMPORTER:      Name: a.dll
# IMPORTER-NEXT: ImportLookupTableRVA:
# IMPORTER-NEXT: ImportAddressTableRVA:
# IMPORTER:      Symbol: LdrAddRefDll
# IMPORTER:      cmpl $0x6c8e9cf5, 0x30(%rdx)
# IMPORTER:      movq (%r13), %r13
# RUN: llvm-objdump -s -j .rdata c.dll | FileCheck --check-prefix=IMPLIST %s
# IMPLIST: b65fec27 d80a3472 {{[0-9a-f]+}} 03000000

## -Bsymbolic keeps the DLL's references local: no cells, no search, and,
## without a delay-loaded DLL, no table.
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj x.lib ntdll.lib \
# RUN:   -Bsymbolic -out:bsym.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 bsym.dll | FileCheck --check-prefix=NONE %s
# RUN: llvm-readobj --coff-imports bsym.dll | \
# RUN:   FileCheck --check-prefix=NOSEARCH %s
# NONE:         000030 00000000 00000000
# NOSEARCH-NOT: LdrAddRefDll

## An executable, which the search never probes, has no table.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   x.lib -delayload:x.dll -out:main.exe
# RUN: od -A x -t x4 -j 0x30 -N 8 main.exe | FileCheck --check-prefix=NONE %s

## The search pins what it binds to.
# RUN: not lld-link -import-slots -dll -noentry a.obj crt.obj x.lib \
# RUN:   protect.lib -out:err.dll 2>&1 | FileCheck --check-prefix=ERR %s
# ERR: error: the image binds cells by name at run time, which needs LdrAddRefDll from ntdll.lib

#--- a.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl fa
fa:
  movq __imp__ZN1n1gE(%rip), %rax
  callq *__imp_fx(%rip)
  retq
  .section .drectve,"yni"
  .ascii " -export:fa"

#--- c.s
  .text
  .globl fc
fc:
  movq __imp__ZN1n1gE(%rip), %rax
  retq
  .section .drectve,"yni"
  .ascii " -export:fc"

#--- x.s
  .text
  .globl fx
fx:
  retq
  .section .drectve,"yni"
  .ascii " -export:fx"

#--- main.s
  .text
  .globl main
main:
  callq *__imp_fx(%rip)
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
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  NtProtectVirtualMemory

#--- protect.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory
