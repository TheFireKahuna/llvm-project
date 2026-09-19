# REQUIRES: x86

# The loader fills in-place import slots in static data only. A reference
# from code to imported data that the compiler took for local is an error, as
# is a read-only slot in a section that cannot be laid out with the import
# address table, and an addend in an image without startup code that applies
# it. Without -import-slots the symbol is simply undefined.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:variable,DATA -implib:%t.lib.lib

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/code.s -o %t.code.obj
# RUN: not lld-link -import-slots -entry:main -subsystem:console -out:%t.code.exe %t.code.obj %t.lib.lib 2>&1 | FileCheck --check-prefix=CODE %s
# CODE: error: {{.*}}code.obj: variable is imported, but is referenced as if it were local; mark its declaration as imported, or compile with -fauto-import

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/section.s -o %t.section.obj
# RUN: not lld-link -import-slots -entry:main -subsystem:console -out:%t.section.exe %t.section.obj %t.lib.lib 2>&1 | FileCheck --check-prefix=SECTION %s
# SECTION: error: {{.*}}section.obj: section .myro holds the address of variable, imported from import-slots-errors.s.tmp.lib.dll, but is read-only and cannot be laid out with the import address table; make the section writable or take the address in code

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/addend.s -o %t.addend.obj
# RUN: not lld-link -import-slots -entry:main -subsystem:console -out:%t.addend.exe %t.addend.obj %t.lib.lib 2>&1 | FileCheck --check-prefix=ADDEND %s
# ADDEND: error: {{.*}}addend.obj: static data holds the address of variable, imported from import-slots-errors.s.tmp.lib.dll, plus 8; the loader writes the plain address, and the image has no startup code that applies the offset (__import_fixups_start)

# RUN: not lld-link -entry:main -subsystem:console -out:%t.plain.exe %t.addend.obj %t.lib.lib 2>&1 | FileCheck --check-prefix=PLAIN %s
# PLAIN: error: undefined symbol: variable

#--- lib.s
.data
.globl variable
variable:
  .quad 1
  .quad 2

#--- code.s
.text
.globl main
main:
  movq variable(%rip), %rax
  ret

#--- section.s
.text
.globl main
main:
  ret
.section .myro,"dr"
.p2align 3
ptr:
  .quad variable

#--- addend.s
.text
.globl main
main:
  ret
.data
ptr:
  .quad variable+8
