# REQUIRES: x86
## Under -import-slots, an import thunk whose every reference is an in-place
## import slot or an instruction rewritten to load the import address table
## entry is left out of the image. A call, a GC root or an export keeps it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   -include:f3 -export:f4 -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s

## Thunks remain for f2 (called), f3 (a GC root) and f4 (exported), not for
## f1, whose address code loads from its import address table entry.
# CHECK:      <.text>:
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#%x,F1:]]
# CHECK-NEXT: callq 0x[[#%x,T2:]]
# CHECK:      [[#%x,T2]]: ff 25 {{.*}} # 0x[[#F1+8]]
# CHECK-NEXT: int3
# CHECK-NOT:  # 0x[[#F1]]
# CHECK:      ff 25 {{.*}} # 0x[[#F1+16]]
# CHECK:      ff 25 {{.*}} # 0x[[#F1+24]]

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  f2
  f3
  f4

#--- main.s
  .text
  .globl main
main:
  leaq f1(%rip), %rax
  callq f2
  retq

  .data
  .quad f1
  .quad f2
  .quad f3
  .quad f4
