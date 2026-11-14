# REQUIRES: x86

## Automatic import does not depend on the order of the symbol table's hashing:
## the members defining __imp_ symbols load in the order the inputs refer to
## them, and under -force:unresolved no X is imported from the placeholder that
## replaces an unresolved __imp_X. An __imp_X that is neither an import nor a
## regular definition is reported, not imported.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple=x86_64-windows-gnu main.s -filetype=obj -o main.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu imps.s -filetype=obj -o imps.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu abs.s -filetype=obj -o abs.obj
# RUN: echo '.data; .globl __imp_ccc; __imp_ccc: .quad 3' | llvm-mc -triple=x86_64-windows-gnu -filetype=obj -o ccc.obj
# RUN: echo '.data; .globl __imp_aaa; __imp_aaa: .quad 1' | llvm-mc -triple=x86_64-windows-gnu -filetype=obj -o aaa.obj
# RUN: echo '.data; .globl __imp_bbb; __imp_bbb: .quad 2' | llvm-mc -triple=x86_64-windows-gnu -filetype=obj -o bbb.obj
# RUN: llvm-ar rcs lib.a ccc.obj aaa.obj bbb.obj

# RUN: lld-link -lldmingw -entry:main -out:out.exe main.obj lib.a -verbose 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ORDER
# ORDER:      Loading lazy __imp_ccc from lib.a for automatic import
# ORDER-NEXT: Loading lazy __imp_aaa from lib.a for automatic import
# ORDER-NEXT: Loading lazy __imp_bbb from lib.a for automatic import

# RUN: lld-link -lldmingw -entry:main -out:out.exe imps.obj -force:unresolved \
# RUN:   2>&1 | FileCheck %s --check-prefix=FORCE \
# RUN:   --implicit-check-not="unable to automatically import"
# FORCE-COUNT-8: warning: undefined symbol:

# RUN: not lld-link -lldmingw -entry:main -out:out.exe abs.obj 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ABS
# ABS: warning: unable to automatically import foo from __imp_foo from <internal>; unexpected symbol type
# ABS: error: undefined symbol: foo

#--- main.s
  .globl main
main:
  movl ccc(%rip), %eax
  movl aaa(%rip), %eax
  movl bbb(%rip), %eax
  ret
  .globl _pei386_runtime_relocator
_pei386_runtime_relocator:
  ret

#--- imps.s
  .globl main
main:
  movl aaa(%rip), %eax
  movq __imp_aaa(%rip), %rax
  movl bbb(%rip), %eax
  movq __imp_bbb(%rip), %rax
  movl ccc(%rip), %eax
  movq __imp_ccc(%rip), %rax
  movl ddd(%rip), %eax
  movq __imp_ddd(%rip), %rax
  ret

#--- abs.s
  .globl main
main:
  movl foo(%rip), %eax
  ret
  .globl __imp_foo
__imp_foo = 0x1000
