# REQUIRES: x86
## Under -import-slots, code takes an imported function's address as static
## data holds it. A described lea of an import thunk becomes a load of the
## import address table entry (C6b), and a described load of a delay-loaded
## function's entry becomes the lea of its thunk, which static data holds. An
## object that may take a function's address in an instruction it does not
## describe makes the image use the thunk as its address everywhere, its words
## in static data included, with a warning (C6c). Calls are unchanged.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium desc.s -o desc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc undesc.s -o undesc.obj
# RUN: yaml2obj bytes.yaml -o bytes.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc abs.s -o abs.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -def:b.def -out:b.lib -machine:x64

# RUN: lld-link -import-slots -entry:main -subsystem:console desc.obj \
# RUN:   undesc.obj a.lib b.lib -delayload:b.dll -out:code.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d code.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports --coff-basereloc code.exe | \
# RUN:   FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-objdump -s -j .data code.exe | FileCheck --check-prefix=DATA %s

# WARN:     warning: undesc.obj: may take the address of f3, imported from a.dll, in an instruction it does not describe, so the image uses its import thunk as its address; declare it imported, or rebuild the object with clang
# WARN-NOT: warning

# CHECK:      <.text>:
# CHECK-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,F1:]]
# CHECK-NEXT: e8 {{.*}} callq 0x[[#%x,T1:]]
# CHECK-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx # 0x[[#F1+8]]
# CHECK-NEXT: 48 8d 15 {{.*}} leaq {{.*}}(%rip), %rdx # 0x[[#%x,T3:]]
# CHECK-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#%x,TG:]]
# CHECK-NEXT: ff 15 {{.*}} callq *{{.*}}(%rip)
# CHECK:      48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#T3]]
# CHECK:      [[#%x,T1]]: ff 25 {{.*}} jmpq *{{.*}}(%rip) # 0x[[#F1]]
# CHECK:      [[#%x,T3]]: ff 25 {{.*}} jmpq *{{.*}}(%rip) # 0x[[#F1+16]]
# CHECK:      [[#%x,TG]]: ff 25

## f1 is in place; f3's word holds its thunk, as does g's.
# IMPORTS:      Name: a.dll
# IMPORTS:      Name: a.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA: 0x3000
# IMPORTS-NEXT: Symbol: f1 (0)
# IMPORTS-NEXT: }
# IMPORTS:      BaseReloc [
# IMPORTS-NOT:    Address: 0x3000
# IMPORTS:        Address: 0x3008
# IMPORTS-NEXT: }
# IMPORTS-NEXT: Entry {
# IMPORTS-NEXT:   Type: DIR64
# IMPORTS-NEXT:   Address: 0x3010

# DATA: 140003000 f0200000 00000000 60100040 01000000
# DATA: 140003010 70100040 01000000

## An instruction that reads the bytes of an imported function, and its
## address in code, cannot be served by a thunk.
# RUN: not lld-link -import-slots -entry:main -subsystem:console desc.obj \
# RUN:   bytes.obj abs.obj a.lib b.lib -delayload:b.dll -out:err.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=ERR %s
# ERR-DAG: error: bytes.obj: f4 is imported from a.dll, but an instruction in .text reads its bytes, which its import thunk's are not
# ERR-DAG: error: abs.obj: .text is executable and holds the address of f4, imported from a.dll

## Without -import-slots, nothing is rewritten.
# RUN: lld-link -entry:main -subsystem:console desc.obj undesc.obj a.lib \
# RUN:   b.lib -delayload:b.dll -out:plain.exe
# RUN: llvm-objdump -d plain.exe | FileCheck --check-prefix=PLAIN %s
# PLAIN:      <.text>:
# PLAIN-NEXT: 48 8d 05 {{.*}} leaq
# PLAIN:      48 8b 05 {{.*}} movq

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  f2
  f3
  f4

#--- b.def
LIBRARY b.dll
EXPORTS
  g

#--- desc.s
  .text
  .globl main
main:
  leaq f1(%rip), %rax
  callq f1
  leaq f2(%rip), %rcx
  leaq f3(%rip), %rdx
  movq __imp_g(%rip), %rax
  callq *__imp_g(%rip)
  retq
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

  .data
  .quad f1
  .quad f3
  .quad g

#--- undesc.s
  .text
  .globl other
other:
  leaq f3(%rip), %rax
  retq

#--- bytes.yaml
## cmpb $1, f4(%rip)
--- !COFF
header:
  Machine: IMAGE_FILE_MACHINE_AMD64
sections:
  - Name: .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment: 16
    SectionData: 803D0000000001C3
    Relocations:
      - VirtualAddress: 2
        SymbolName: f4
        Type: IMAGE_REL_AMD64_REL32_1
symbols:
  - Name: f4
    Value: 0
    SectionNumber: 0
    SimpleType: IMAGE_SYM_TYPE_NULL
    ComplexType: IMAGE_SYM_DTYPE_NULL
    StorageClass: IMAGE_SYM_CLASS_EXTERNAL
...

#--- abs.s
  .text
  movabsq $f4, %rax
  retq
