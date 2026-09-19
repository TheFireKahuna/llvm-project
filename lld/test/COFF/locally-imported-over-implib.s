# REQUIRES: x86

# With -import-slots, an import-form reference to a symbol that a regular
# object defines binds to that definition even when an import library in the
# link offers the symbol: the import library's member is not fetched, so the
# definition under the plain name meets no duplicate, and the reference is
# rewritten to reach it. A symbol nothing in the link defines still comes
# from the import library. Without -import-slots the member is fetched and
# the definitions clash, as with link.exe.

# RUN: split-file %s %t.dir
# RUN: yaml2obj %p/Inputs/export.yaml -o %t.exp.obj
# RUN: lld-link -out:%t.exp.dll -dll %t.exp.obj -export:exportfn1 -export:exportfn2 -implib:%t.exp.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: not lld-link -entry:main -subsystem:console -out:%t.dup.exe %t.main.obj %t.exp.lib 2>&1 | FileCheck --check-prefix=DUP %s
# RUN: lld-link -import-slots -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.main.obj %t.exp.lib 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
# RUN: lld-link -import-slots -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.main.obj -defaultlib:%t.exp.lib 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
# RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports %t.exe | FileCheck --check-prefix=IMPORTS %s

# DUP: error: duplicate symbol: exportfn1

# QUIET-NOT: duplicate symbol
# QUIET-NOT: locally defined symbol imported

# CHECK:      <main>:
# CHECK-NEXT:   addr32 callq 0x{{[0-9a-f]+}} <exportfn1>
# CHECK-NEXT:   callq *0x{{[0-9a-f]+}}(%rip)
# CHECK-NEXT:   retq
# CHECK:      <exportfn1>:
# CHECK-NEXT:   retq

# IMPORTS:      Symbol: exportfn2
# IMPORTS-NOT:  Symbol: exportfn1

#--- main.s
.text
.globl main
main:
  call *__imp_exportfn1(%rip)
  call *__imp_exportfn2(%rip)
  ret

.globl exportfn1
exportfn1:
  ret
