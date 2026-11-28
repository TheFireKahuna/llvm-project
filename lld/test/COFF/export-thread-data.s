# REQUIRES: x86
# RUN: split-file %s %t.dir && cd %t.dir

# link.exe's /experimental:tlsDllInterface: /export:X,THREAD_DATA exports X's
# offset in the loader's template as __tls_offset_X, and the image's
# __safe_get_tls_index and _tls_index, which an importer's access reads.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc exporter.s -o exporter.obj
# RUN: lld-link -dll -noentry exporter.obj -out:exporter.dll
# RUN: llvm-readobj --coff-exports exporter.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: llvm-objdump -s --section=.rdata exporter.dll | FileCheck --check-prefix=OFFSETS %s

# EXPORTS:      Name: __tls_offset_x
# EXPORTS-NEXT: RVA: 0x2000
# EXPORTS:      Name: __tls_offset_y
# EXPORTS-NEXT: RVA: 0x2004
# EXPORTS:      Name: __tls_safe_get_tls_index
# EXPORTS:      Name: _tls_index

# x and y follow the 4 bytes of _tls_start's group.
# OFFSETS: 180002000 04000000 08000000

# The importer binds __imp___tls_safe_get_tls_index_X to the function of the
# DLL that offers __tls_offset_X, one import for both variables.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc importer.s -o importer.obj
# RUN: lld-link -dll -noentry importer.obj exporter.lib -out:importer.dll
# RUN: llvm-readobj --coff-imports importer.dll | FileCheck --check-prefix=IMPORTS %s

# IMPORTS:      Name: exporter.dll
# IMPORTS:      Symbol: __tls_offset_x
# IMPORTS-NEXT: Symbol: __tls_offset_y
# IMPORTS-NEXT: Symbol: __tls_safe_get_tls_index
# IMPORTS-NEXT: }

# Code of the owned model reaches no such variable; the error says why.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium fork.s -o fork.obj
# RUN: not lld-link -dll -noentry fork.obj exporter.lib -out:fork.dll 2>&1 | FileCheck --check-prefix=FORK %s
# FORK:      error: undefined symbol: __declspec(dllimport) x$tls
# FORK:      NOTE: exporter.lib offers x as a thread-local of the loader's template, through link.exe's /experimental:tlsDllInterface (__tls_offset_x), which only foreign code can reach.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc notls.s -o notls.obj
# RUN: not lld-link -dll -noentry exporter.obj notls.obj -out:notls.dll 2>&1 | FileCheck --check-prefix=ERR %s
# ERR: error: /export:z,THREAD_DATA names no thread-local variable

#--- exporter.s
  .section .tls,"dw"
  .globl _tls_start
_tls_start:
  .long 0
  .section .tls$,"dw"
  .globl x, y
x:
  .long 1
y:
  .long 2
  .data
  .globl _tls_index
_tls_index:
  .long 0
  .text
  .globl __safe_get_tls_index
__safe_get_tls_index:
  movl _tls_index(%rip), %eax
  retq
  .section .drectve,"yni"
  .ascii " /EXPORT:x,THREAD_DATA /EXPORT:y,THREAD_DATA"

#--- importer.s
  .text
  .globl f
f:
  callq *__imp___tls_safe_get_tls_index_x(%rip)
  movq __imp___tls_offset_x(%rip), %rax
  callq *__imp___tls_safe_get_tls_index_y(%rip)
  movq __imp___tls_offset_y(%rip), %rax
  retq

#--- fork.s
  .text
  .globl g
g:
  movq __imp_x$tls(%rip), %rax
  retq

#--- notls.s
  .data
  .globl z
z:
  .long 3
  .section .drectve,"yni"
  .ascii " /EXPORT:z,THREAD_DATA"
