# REQUIRES: x86

## Under -import-slots, an import library's offer satisfies an extern_weak
## reference, as a shared object satisfies an ELF weak reference, though an
## archive member does not; its .refptr.X pointer then becomes X's import
## pointer, which every reader reads in its place. A reference to weak data
## that is imported, other than through its pointer, is an error, since the
## data has no address in the image.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium data.s -o data.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-lib -out:lib.lib lib.obj
# RUN: lld-link -dll -noentry -out:dll.dll -implib:dll.lib dll.obj \
# RUN:   -export:f -export:v,DATA

# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj dll.lib lib.lib
# RUN: llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s
# RUN: llvm-objdump -d a.exe | FileCheck %s

# IMPORTS:      Name: dll.dll
# IMPORTS:      Symbol: f
# IMPORTS-NEXT: Symbol: v

## No pointer is left; the archive's g is not loaded, so g is zero.
# RELOC-NOT: Type: DIR64

# CHECK:      48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,IATF:]]
# CHECK-NEXT: ff 15 {{.*}} callq *{{.*}}(%rip) # 0x[[#IATF]]
# CHECK-NEXT: 48 83 3d {{.*}} cmpq $0x0, {{.*}}(%rip) # 0x[[#IATF + 8]]
# CHECK-NEXT: 48 c7 c1 00 00 00 00 movq $0x0, %rcx

## Without -import-slots no member satisfies a weak reference.
# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj dll.lib \
# RUN:   lib.lib
# RUN: llvm-readobj --coff-imports b.exe | FileCheck --check-prefix=NOIMPORTS %s

# NOIMPORTS-NOT: dll.dll

# RUN: not lld-link -import-slots -entry:main -subsystem:console -out:c.exe \
# RUN:   main.obj data.obj dll.lib 2>&1 | FileCheck --check-prefix=ERR %s

# ERR: error: data.obj: weak reference to v, which is imported, needs its address in .data

#--- main.s
  .text
  .globl main
main:
  movq .refptr.f(%rip), %rax
  callq *.refptr.f(%rip)
  cmpq $0, .refptr.v(%rip)
  movq .refptr.g(%rip), %rcx
  retq

  .weak f
  .weak v
  .weak g

.macro refptr sym
  .section .rdata$.refptr.\sym,"dr",discard,.refptr.\sym
  .globl .refptr.\sym
.refptr.\sym:
  .quad \sym
.endm
  refptr f
  refptr v
  refptr g

#--- data.s
  .data
  .globl p
p:
  .quad v
  .weak v

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
