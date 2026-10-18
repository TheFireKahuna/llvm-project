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
## module. The DLL itself is not pinned: the C runtime keeps it loaded only
## once it registers the destructor of an instance another image owns.

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

## The table: its magic, flags and two buckets, the mask of a one-word
## filter, one delay-loaded DLL's key at 0x40 and the buckets at 0x28; the
## filter word, then the bucket of _ZN1n1gE's key, which holds the key's top
## half and names its pointer and the name its export holds, a free bucket,
## and x.dll's key after the buckets.
# CHECK:      000030 6c8e9cf5 [[#%x,TABLE:]]
# CHECK:      [[#TABLE + 0x180000000]] 2b1d7a4f 00000000 02000000 00000000
# CHECK-NEXT: 00000000 01000000 40000000 28000000
# CHECK-NEXT: {{[0-9a-f]+}} {{[0-9a-f]+}} d80a3472 {{[0-9a-f]+}}
# CHECK-NEXT: {{[0-9a-f]+}} 00000000 00000000 00000000
# CHECK-NEXT: 5ac27ce3 f11f7492

# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,NTDLL:]]
# CHECK-NEXT: Symbol: LdrAddRefDll
# CHECK-NEXT: Symbol: NtProtectVirtualMemory

## Bit 2 of r14 records a DLL loaded after the process initialized. For an
## entity the executable's table does not name, such a DLL searches the images
## of its scope, the executable first through the table it names at 0x38, then
## each module with a table's tag inside its image, probed if loaded with the
## process, if delay-loaded within that scope, or if it delay-loads this DLL,
## by the key of this DLL's name.
# CHECK:      pushq %rbx
# CHECK:      xorl %r14d, %r14d
# CHECK-NEXT: movq %gs:0x60, %rax
# CHECK-NEXT: testb $0x2, 0x50(%rax)
# CHECK-NEXT: jne
# CHECK-NEXT: orl $0x4, %r14d
# CHECK-NEXT: movq 0x10(%rax), %rdi
# CHECK-NEXT: cmpl $0x7b3ac2d5, 0x80(%rdi)
# CHECK:      movq (%rbx), %r12
# CHECK-NEXT: andq $-0x2, %r12
# CHECK:      cmpl $0x1000, 0xc(%rbx)
# CHECK-NEXT: jb
# CHECK-NEXT: btl $0x2, %r14d
# CHECK-NEXT: jae
# CHECK-NEXT: movq %gs:0x60, %rax
# CHECK-NEXT: movq 0x18(%rax), %rbp
# CHECK-NEXT: addq $0x10, %rbp
# CHECK-NEXT: movq (%rbp), %r15
# CHECK:      movq 0x30(%r15), %rdx
# CHECK:      cmpq (%rbp), %r15
# CHECK-NEXT: jne
# CHECK-NEXT: movl 0x38(%rdx), %r8d
# CHECK:      cmpl $0x6c8e9cf5, 0x30(%rdx)
# CHECK:      movl 0x34(%rdx), %r8d
# CHECK-NEXT: movl 0x40(%r15), %r10d
# CHECK-NEXT: cmpl %r10d, %r8d
# CHECK:      cmpl $0x4f7a1d2b, (%r8)
# CHECK:      testb $0x20, 0x68(%r15)
## A delay-loaded module counts if its importer, by the base the loader
## records, is the executable or loaded with the process, in turn.
# CHECK:      cmpl $0x3, 0x10c(%rax)
# CHECK:      movq 0xb8(%rax), %r9
# CHECK:      cmpq %r9, 0x30(%rax)
# CHECK:      cmpq (%rbp), %rax
# CHECK:      testb $0x20, 0x68(%rax)
# CHECK:      movabsq $0x58a7bdf45370db42, %rax
## Two of the key's bits in the filter's word, then the buckets.
# CHECK:      andl 0x10(%r8), %eax
# CHECK-NEXT: movq 0x20(%r8,%rax,8), %rax
# CHECK:      btq %rcx, %rax
# CHECK:      btq %rcx, %rax
## A bucket's key counts only if its name, inside the image, is the
## entity's, up to its terminator.
# CHECK:      movzbl (%rax), %r11d
# CHECK-NEXT: cmpb (%r9), %r11b
# CHECK-NEXT: jne
# CHECK-NEXT: testb %r11b, %r11b
## The executable's entry names the instance itself, a DLL's a word holding
## it. On a hit in a DLL, LdrAddRefDll(LDR_ADDREF_DLL_PIN, module), whose
## failure ends the search with its status; the executable is never unloaded.
## The pointer is written between two protection calls.
# CHECK:      leaq (%rdx,%rax), %r13
# CHECK-NEXT: testb $0x1, 0x4(%r8)
# CHECK-NEXT: jne
# CHECK-NEXT: movq (%r13), %r13
# CHECK:      cmpq (%rbp), %r15
# CHECK-NEXT: je
# CHECK-NEXT: movl $0x1, %ecx
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,NTDLL + 0x180000000]]
# CHECK-NEXT: testl %eax, %eax
# CHECK-NEXT: js
# CHECK:      testb $0x1, (%rbx)
# CHECK:      movl $0x4, %r9d
# CHECK:      movq %r13, (%rcx,%rax)
# CHECK-NEXT: addq $0x10, %rbx
## An entity the table names takes the word its entry names, then the
## instance from it.
# CHECK:      movq 0x8(%rax), %r13
# CHECK-NEXT: cmpl $0x1, 0xc(%rbx)
# CHECK-NEXT: je
# CHECK-NEXT: cmpl $0x1000, 0xc(%rbx)
# CHECK-NEXT: jb
# CHECK-NEXT: movq (%r13), %r13
# CHECK-NEXT: jmp
# CHECK:      popq %rbx
# CHECK-NEXT: retq

## A jump of the search takes an 8-bit displacement where that reaches, and a
## 32-bit one only where it does not.
# RUN: FileCheck --check-prefix=JUMP --input-file=a.txt %s
# JUMP:      cmpq %rsi, %rbx
# JUMP-NEXT: 0f 83 {{.*}} jae
# JUMP:      cmpq (%rbp), %r15
# JUMP-NEXT: 75 {{[0-9a-f]+}} jne

## Nothing pins the DLL itself.
# RUN: FileCheck --check-prefix=NOSELF --input-file=a.txt %s
# NOSELF-NOT: leaq {{.*}}(%rip), %rdx # 0x180000000

## The fill's list holds the pointer, read-only, with its name.
# RUN: llvm-objdump -s -j .rdata a.dll | FileCheck --check-prefix=BIND %s
# BIND: b75fec27 d80a3472 {{[0-9a-f]+}} {{[0-9a-f]*[2-9a-f][0-9a-f]*}}

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
# IMPLIST: b75fec27 d80a3472 {{[0-9a-f]+}} {{[0-9a-f]*[2-9a-f][0-9a-f]*}}

## -Bsymbolic keeps the DLL's references local: no cells, no search, and,
## without a delay-loaded DLL, no table.
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj x.lib ntdll.lib \
# RUN:   -Bsymbolic -out:bsym.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 bsym.dll | FileCheck --check-prefix=NONE %s
# RUN: llvm-readobj --coff-imports bsym.dll | \
# RUN:   FileCheck --check-prefix=NOSEARCH %s
# NONE:         000030 00000000 00000000
# NOSEARCH-NOT: LdrAddRefDll

## An executable's table, which the MZ header names by its RVA at 0x38, holds
## the vague entities it exports, the instances it shares; its flag says that
## each entry names the instance itself.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium owner.s -o owner.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console owner.obj crt.obj \
# RUN:   -out:owner.exe
# RUN: od -A x -t x4 -j 0x38 -N 4 owner.exe > owner.txt
# RUN: llvm-readobj --coff-exports owner.exe >> owner.txt
# RUN: llvm-objdump -s -j .rdata owner.exe >> owner.txt
# RUN: FileCheck --check-prefix=EXE --input-file=owner.txt %s
# EXE:      000038 [[#%x,ETABLE:]]
# EXE:      Name: _ZN1n1gE
# EXE-NEXT: RVA: 0x3000
# EXE:      [[#ETABLE + 0x140000000]] 2b1d7a4f 01000000 02000000 00000000
# EXE-NEXT: 00000000 00000000 40000000 28000000
## The bucket names the instance, at the export's RVA.
# EXE-NEXT: {{[0-9a-f]+}} {{[0-9a-f]+}} d80a3472 00300000

## An executable that exports no vague entity has no table there.

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   x.lib -delayload:x.dll -out:main.exe
# RUN: od -A x -t x4 -j 0x30 -N 8 main.exe | FileCheck --check-prefix=NONE %s

## The search pins what it binds to.
# RUN: not lld-link -import-slots -dll -noentry a.obj crt.obj x.lib \
# RUN:   protect.lib -out:err.dll 2>&1 | FileCheck --check-prefix=ERR %s
# ERR: error: the image's fill needs LdrAddRefDll from ntdll.lib

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

#--- owner.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl main
main:
  retq
  .section .drectve,"yni"
  .ascii " -export:_ZN1n1gE,DATA"

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
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

#--- protect.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory
