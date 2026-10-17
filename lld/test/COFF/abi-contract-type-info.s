# REQUIRES: x86
# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium k.s -o k.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium typeid.s \
# RUN:   -o typeid.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium call.s -o call.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium other.s -o other.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s \
# RUN:   -o helper.obj

# A DLL that exports a type_info object it owns exports it under the name its
# type's contract qualifies too, in place of the type's witness, and its import
# library binds the object to that name. A type_info object it offers, which it
# does not own, keeps its plain name and its type its witness.
# RUN: lld-link -import-slots -dll -noentry k.obj -out:k.dll
# RUN: llvm-objdump -p k.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: llvm-readobj k.lib | FileCheck --check-prefix=IMPLIB %s
# EXPORTS:      Ordinal      RVA  Name
# EXPORTS-NEXT:       1 {{.*}}  _ZTI1K
# EXPORTS-NEXT:       2 {{.*}}  _ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA
# EXPORTS-NEXT:       3 {{.*}}  _ZTI1V
# EXPORTS-NEXT:       4 {{.*}}  __abiw$0un0mrdTsFA$Ri2bH57FX7rnT6fFnGiSmQ
# EXPORTS-NEXT:       5 {{.*}}  makeK
# EXPORTS-EMPTY:
# IMPLIB:      Name type: export as
# IMPLIB-NEXT: Export name: _ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA
# IMPLIB-NEXT: Symbol: __imp__ZTI1K{{$}}
# IMPLIB:      Type: code
# IMPLIB-NEXT: Name type: export as
# IMPLIB-NEXT: Export name: _ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA
# IMPLIB-NEXT: Symbol: __imp__ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA$k.dll

# An image that imports the object binds it by that name, which checks the
# contract its code requires of the DLL with no import of its own.
# RUN: lld-link -import-slots -entry:main -subsystem:console typeid.obj k.lib \
# RUN:   -out:typeid.exe
# RUN: llvm-readobj --coff-imports typeid.exe | \
# RUN:   FileCheck --check-prefix=TYPEID %s
# TYPEID:      Symbol: _ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA (1)
# TYPEID-NEXT: Symbol: makeK (4)
# TYPEID-NEXT: }

# An image that requires the contract without importing the object imports
# the qualified name as a witness, which a delay-load descriptor can hold.
# RUN: lld-link -import-slots -entry:main -subsystem:console call.obj k.lib \
# RUN:   -out:call.exe
# RUN: llvm-readobj --coff-imports call.exe | FileCheck --check-prefix=CALL %s
# RUN: lld-link -import-slots -entry:main -subsystem:console call.obj \
# RUN:   helper.obj k.lib -delayload:k.dll -out:delay.exe
# RUN: llvm-readobj --coff-imports delay.exe | \
# RUN:   FileCheck --check-prefix=DELAY %s
# CALL:      Symbol: _ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA (1)
# CALL-NEXT: Symbol: makeK (4)
# DELAY:      DelayImport {
# DELAY:        Symbol: _ZTI1K$abi$siyGeWQI3ir6d0tXhbGrAA (1)
# DELAY:        Symbol: makeK (4)

# A requirement of another layout is an error.
# RUN: not lld-link -import-slots -entry:main -subsystem:console other.obj \
# RUN:   k.lib -out:other.exe 2>&1 | FileCheck --check-prefix=OTHER %s
# OTHER: error: ABI mismatch for 'K': other.obj was compiled with a different layout than k.dll exports (needed because other.obj uses makeK)

# The names are reserved.
# RUN: not lld-link -import-slots -dll -noentry k.obj \
# RUN:   '-export:_ZTI1K$abi$x=makeK' -out:reserved.dll 2>&1 \
# RUN:   | FileCheck --check-prefix=RESERVED %s
# RESERVED: error: cannot export makeK as _ZTI1K$abi$x: the name is reserved for ABI contract witnesses

#--- k.s
  .linkbindings
  .section .rdata,"dr"
  .globl _ZTI1K
  .p2align 3
_ZTI1K:
  .quad 0
  .quad _ZTS1K
_ZTS1K:
  .asciz "1K"

  .section .rdata,"dr",discard,_ZTI1V
  .globl _ZTI1V
  .p2align 3
_ZTI1V:
  .linkbinding _ZTI1V, default
  .quad 0
  .quad 0

  .text
  .globl makeK
makeK:
  leaq _ZTI1V(%rip), %rax
  retq
  .section .drectve,"yn"
  .ascii " -export:makeK -export:_ZTI1K,data"
  .ascii " /abiprovide:yOgEiMW-Dx8siyGeWQI3ir6d0tXhbGrAA,0un0mrdTsFARi2bH57FX7rnT6fFnGiSmQ"

#--- typeid.s
  .text
  .globl main
main:
  .linkcontract main, __imp_makeK, type, "1K", "b22c86796408de2afa774b5785b1ab00"
  callq *__imp_makeK(%rip)
  movq __imp__ZTI1K(%rip), %rax
  retq

#--- call.s
  .text
  .globl main
main:
  .linkcontract main, __imp_makeK, type, "1K", "b22c86796408de2afa774b5785b1ab00"
  callq *__imp_makeK(%rip)
  retq

#--- other.s
  .text
  .globl main
main:
  .linkcontract main, __imp_makeK, type, "1K", "000102030405060708090a0b0c0d0e0f"
  callq *__imp_makeK(%rip)
  retq

#--- helper.s
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq
