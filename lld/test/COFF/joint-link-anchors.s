# REQUIRES: x86
## In a joint link, a load of the import pointer of a member DLL's data whose
## pointer the register of a load of the pointer of other data of that DLL
## holds becomes an lea of the distance between the data, which the DLL's
## import library records with its layout. The DLL exports its identity, named
## after that layout, and the importer imports it; the import that only such
## loads read is left out. A pointer to data of another image, or one the
## register does not hold, is still loaded.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj a.lib \
# RUN:   b.lib -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan app.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj a.lib \
# RUN:   b.lib -out:app.exe -lldjoint:facts/app.exe.jfacts

# RUN: llvm-readobj --coff-exports a.dll > out.txt
# RUN: llvm-readobj --coff-imports app.exe >> out.txt
# RUN: FileCheck --check-prefix=EXPORTS --input-file=out.txt %s
# EXPORTS:      Name: __llvm_private$[[ID:[0-9a-f]{16}]]
# EXPORTS:      Name: x
# EXPORTS-NEXT: RVA: 0x[[#%x,X:]]
# EXPORTS:      Name: y
# EXPORTS-NEXT: RVA: 0x[[#%x,X+8]]
# EXPORTS:      Name: a.dll
# EXPORTS-NEXT: ImportLookupTableRVA:
# EXPORTS-NEXT: ImportAddressTableRVA:
# EXPORTS-NEXT: Symbol: __llvm_private$[[ID]] (0)
# EXPORTS-NEXT: Symbol: x (0)
# EXPORTS-NEXT: }
# EXPORTS:      Name: b.dll
# EXPORTS-NEXT: ImportLookupTableRVA:
# EXPORTS-NEXT: ImportAddressTableRVA:
# EXPORTS-NEXT: Symbol: z (0)
# EXPORTS-NEXT: }
# RUN: llvm-nm a.lib | FileCheck --check-prefix=LIB %s
# LIB-NOT: __llvm_private
# LIB:     __llvm_layout$a.dll
# LIB-NOT: __llvm_private

# RUN: llvm-objdump -d --no-show-raw-insn app.exe | FileCheck --check-prefix=CODE %s
# CODE:      movq {{.*}}(%rip), %rcx
# CODE-NEXT: leaq 0x8(%rcx), %rdx
# CODE-NEXT: movq {{.*}}(%rip), %r8
# CODE-NEXT: movl (%rdx), %eax
# CODE-NEXT: movq {{.*}}(%rip), %rcx
# CODE-NEXT: movq {{.*}}(%rip), %rdx
# CODE-NEXT: retq

#--- a.s
  .data
  .globl x, y
x:
  .quad 1
y:
  .quad 2

  .section .drectve,"yni"
  .ascii " -export:x,DATA -export:y,DATA"

#--- b.s
  .data
  .globl z
z:
  .quad 3

  .section .drectve,"yni"
  .ascii " -export:z,DATA"

#--- app.s
  .text
  .globl main
main:
.Lx:
  movq __imp_x(%rip), %rcx
  .linkanchor .Lx, %rcx
  movq __imp_y(%rip), %rdx
  .linkanchor .Lx, %rcx
  movq __imp_z(%rip), %r8
  movl (%rdx), %eax
.Lx2:
  movq __imp_x(%rip), %rcx
  movq __imp_x(%rip), %rdx
  retq
