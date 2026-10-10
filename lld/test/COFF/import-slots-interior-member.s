# REQUIRES: x86
## Under -import-slots, the names that a word of static data may take for an
## address inside imported data, and NtProtectVirtualMemory for the residual
## fill, are loaded only from an import. A regular archive member that defines
## such a name is not loaded: the word is the residual fill's to write, as if
## nothing defined the name.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium interior.s \
# RUN:   -o interior.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium protect.s \
# RUN:   -o protect.obj
# RUN: llvm-lib -out:interior.lib interior.obj
# RUN: llvm-lib -out:protect.lib protect.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj rw.obj \
# RUN:   interior.lib lib.lib
# RUN: llvm-readobj --coff-imports --sections out.dll | FileCheck %s
# RUN: lld-link -dll -noentry -import-slots -opt:noref -out:noref.dll crt.obj \
# RUN:   rw.obj interior.lib lib.lib
# RUN: llvm-readobj --coff-imports --sections noref.dll | FileCheck %s

# CHECK-NOT:  Name: .member
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: obj ({{[0-9]+}})
# CHECK-NEXT: }
# CHECK-NOT:  Symbol:

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj \
# RUN:   protect.lib lib.lib 2>&1 | FileCheck --check-prefix=PROTECT %s

# PROTECT: error: ro.obj: .rdata holds the address of obj plus 8, imported from lib.dll, which exports no name for it, and the image cannot make it read-only once written without NtProtectVirtualMemory from ntdll.lib

#--- lib.def
LIBRARY lib.dll
EXPORTS
  obj DATA

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

#--- rw.s
  .data
  .p2align 3
  .quad obj+8

#--- ro.s
  .section .rdata,"dr"
  .p2align 3
  .quad obj+8

#--- interior.s
  .section .member,"dw"
  .globl "__imp_obj$so8"
"__imp_obj$so8":
  .quad 0

#--- protect.s
  .section .member,"dw"
  .globl __imp_NtProtectVirtualMemory
__imp_NtProtectVirtualMemory:
  .quad 0
