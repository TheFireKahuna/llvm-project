# REQUIRES: x86

## The loader writes the security cookie as it maps the image, and the import
## address table's pages as it binds the imports, so the cookie's chunk goes on
## a page of the table: just after it, or before it where it ends a page.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium cookie.s -o cookie.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill.obj
# RUN: llvm-lib -machine:x64 -def:one.def -out:one.lib
# RUN: %python -c "print('LIBRARY many.dll\nEXPORTS'); [print(' f%%d' %% i) for i in range(511)]" > many.def
# RUN: %python -c "[print('.quad __imp_f%%d' %% i) for i in range(511)]" > many.s
# RUN: llvm-lib -machine:x64 -def:many.def -out:many.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium many.s -o many.obj
# RUN: lld-link -entry:main -subsystem:console main.obj cookie.obj fill.obj \
# RUN:   one.lib -debug:symtab -out:one.exe
# RUN: llvm-readobj --file-headers --syms one.exe | FileCheck --check-prefix=AFTER %s
# RUN: lld-link -entry:main -subsystem:console main.obj cookie.obj fill.obj \
# RUN:   many.obj many.lib -alternatename:__imp_f=__imp_f0 -debug:symtab -out:many.exe
# RUN: llvm-readobj --file-headers --syms many.exe | FileCheck --check-prefix=BEFORE %s

# AFTER:  IATRVA: 0x{{[0-9A-F]+}}000
# AFTER:  IATSize: 0x10
# AFTER:  Name: __security_cookie
# AFTER-NEXT: Value: 16

# BEFORE: IATRVA: 0x{{[0-9A-F]+}}008
# BEFORE: IATSize: 0x1000
# BEFORE: Name: __security_cookie
# BEFORE-NEXT: Value: 0

#--- main.s
  .text
  .globl main
main:
  callq *__imp_f(%rip)
  retq
  .section .rdata,"dr"
  .quad 1

#--- cookie.s
  .section .rdata,"dr"
  .globl __security_cookie
__security_cookie:
  .quad 0x2B992DDFA232

#--- one.def
LIBRARY one.dll
EXPORTS
  f
