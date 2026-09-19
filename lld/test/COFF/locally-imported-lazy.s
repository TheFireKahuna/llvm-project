# REQUIRES: x86

# An import-form reference to a symbol that only an archive member defines
# loads the member, as a direct reference would, and is then rewritten to
# reach it directly. The member may bring further such references.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/f.s -o %t.f.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/g.s -o %t.g.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: llvm-lib %t.f.obj %t.g.obj -out:%t.lib
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.main.obj %t.lib 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
# RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck %s

# QUIET-NOT: locally defined symbol imported
# QUIET-NOT: undefined symbol

# CHECK:      <main>:
# CHECK-NEXT:   addr32 callq 0x{{[0-9a-f]+}} <f>
# CHECK:      <f>:
# CHECK-NEXT:   addr32 callq 0x{{[0-9a-f]+}} <g>
# CHECK:      <g>:
# CHECK-NEXT:   retq

#--- main.s
.text
.globl main
main:
  call *__imp_f(%rip)
  ret

#--- f.s
.text
.globl f
f:
  call *__imp_g(%rip)
  ret

#--- g.s
.text
.globl g
g:
  ret
