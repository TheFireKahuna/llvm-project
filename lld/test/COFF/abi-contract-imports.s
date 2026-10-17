# REQUIRES: x86
# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium d.s -o d.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium e.s -o e.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plain.s -o plain.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium skew.s -o skew.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium gc.s -o gc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s \
# RUN:   -o helper.obj
# RUN: lld-link -import-slots -dll -noentry d.obj -out:d.dll
# RUN: lld-link -import-slots -dll -noentry e.obj -out:e.dll
# RUN: lld-link -import-slots -dll -noentry plain.obj -out:plain.dll

# The code of main.obj requires the contract of T of each DLL it calls into.
# d.dll and e.dll export the same witness, which main.exe imports from both;
# plain.dll vouches for nothing, and main.exe imports nothing more from it.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   d.lib e.lib plain.lib -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=BOTH %s
# BOTH:      Name: d.dll
# BOTH:      Symbol: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw (0)
# BOTH-NEXT: Symbol: g (1)
# BOTH:      Name: e.dll
# BOTH:      Symbol: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw (0)
# BOTH-NEXT: Symbol: h (1)
# BOTH:      Name: plain.dll
# BOTH-NEXT: ImportLookupTableRVA:
# BOTH-NEXT: ImportAddressTableRVA:
# BOTH-NEXT: Symbol: p (0)
# BOTH-NEXT: }

# A delay-loaded DLL's witness is in its delay-load descriptor.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   helper.obj d.lib e.lib plain.lib -delayload:d.dll -out:delay.exe
# RUN: llvm-readobj --coff-imports delay.exe | FileCheck --check-prefix=DELAY %s
# DELAY:      DelayImport {
# DELAY-NEXT:   Name: d.dll
# DELAY:        Import {
# DELAY-NEXT:     Symbol: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw
# DELAY:        Import {
# DELAY-NEXT:     Symbol: g

# A requirement of another contract of T is an error against a DLL that exports
# a witness of T, and nothing against one that exports none.
# RUN: not lld-link -import-slots -entry:main -subsystem:console skew.obj \
# RUN:   d.lib plain.lib -out:skew.exe 2>&1 | FileCheck --check-prefix=SKEW %s
# SKEW:     error: ABI mismatch for 'T': skew.obj was compiled with a different layout than d.dll exports (needed because skew.obj uses g)
# SKEW-NOT: plain.dll

# A requirement of code the link discards requires nothing.
# RUN: lld-link -import-slots -entry:main -subsystem:console -opt:ref gc.obj \
# RUN:   d.lib -out:gc.exe
# RUN: llvm-readobj --coff-imports gc.exe | FileCheck --check-prefix=GC %s
# GC:      Name: d.dll
# GC-NEXT: ImportLookupTableRVA:
# GC-NEXT: ImportAddressTableRVA:
# GC-NEXT: Symbol: g (1)
# GC-NEXT: }

# Within an image the contracts of one identity agree.
# RUN: not lld-link -import-slots -dll -noentry d.obj skew.obj d.lib \
# RUN:   -out:conflict.dll 2>&1 | FileCheck --check-prefix=CONFLICT %s
# CONFLICT: error: ABI contract mismatch for 'T': d.obj and skew.obj were compiled with different layouts

#--- d.s
  .text
  .globl g
g:
  retq
  .section .drectve,"yn"
  .ascii " -export:g /abiprovide:Z7E5CzMxeVEAAECAwQFBgcICQoLDA0ODw"

#--- e.s
  .text
  .globl h
h:
  retq
  .section .drectve,"yn"
  .ascii " -export:h /abiprovide:Z7E5CzMxeVEAAECAwQFBgcICQoLDA0ODw"

#--- plain.s
  .text
  .globl p
p:
  retq
  .section .drectve,"yn"
  .ascii " -export:p"

#--- main.s
  .text
  .globl main
main:
  .linkcontract main, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
  .linkcontract main, __imp_h, type, "1T", "000102030405060708090a0b0c0d0e0f"
  .linkcontract main, __imp_p, type, "1T", "000102030405060708090a0b0c0d0e0f"
  callq *__imp_g(%rip)
  callq *__imp_h(%rip)
  callq *__imp_p(%rip)
  retq

#--- skew.s
  .text
  .globl main
main:
  .linkcontract main, __imp_g, type, "1T", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
  .linkcontract main, __imp_p, type, "1T", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
  callq *__imp_g(%rip)
  callq *__imp_p(%rip)
  retq

#--- gc.s
  .section .text,"xr",one_only,main
  .globl main
main:
  callq *__imp_g(%rip)
  retq
  .section .text,"xr",one_only,unused
  .globl unused
unused:
  .linkcontract unused, __imp_g, type, "1T", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
  callq *__imp_g(%rip)
  retq

#--- helper.s
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq
