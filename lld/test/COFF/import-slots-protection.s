# REQUIRES: x86
## The loader restores one protection over the import address table directory,
## which holds the read-only in-place import slots, so the pages it spans must
## be read-only data; and no slot may be in an executable section. Options that
## change the layout's protection are checked against the final image.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   -out:ok.exe

# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   a.lib -out:rw.exe -section:.rdata,RW 2>&1 | \
# RUN:   FileCheck --check-prefix=RW %s
# RW: error: section .rdata shares a page with the import address table, which holds in-place import slots, but is not read-only data

# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   a.lib -out:x.exe -merge:.rdata=.text 2>&1 | \
# RUN:   FileCheck --check-prefix=EXEC %s
# EXEC: error: main.obj: the address of an import at offset 0x0 in .rdata is in executable section .text
# EXEC: error: section .text shares a page with the import address table, which holds in-place import slots, but is not read-only data

# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   a.lib -out:d.exe -merge:.idata=.data 2>&1 | \
# RUN:   FileCheck --check-prefix=OUTSIDE %s
# OUTSIDE: error: section .data shares a page with the import address table, which holds in-place import slots, but is not read-only data

#--- a.def
LIBRARY a.dll
EXPORTS
  func1

#--- main.s
  .text
  .globl main
main:
  movq ptr(%rip), %rax
  retq

  .section .rdata,"dr"
  .globl ptr
ptr:
  .quad func1
