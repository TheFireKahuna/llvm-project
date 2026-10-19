# REQUIRES: x86
# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium unrelated.s \
# RUN:   -o unrelated.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj

# A DLL exports a witness of each contract that the objects whose definitions
# it exports provide, those of unrelated.obj aside; every witness names one
# byte through one ordinal, and only the import library's members named for
# the DLL import them.
# RUN: lld-link -import-slots -dll -noentry a.obj unrelated.obj -out:a.dll
# RUN: llvm-objdump -p a.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: llvm-readobj a.lib | FileCheck --check-prefix=IMPLIB %s
# RUN: llvm-nm a.lib | FileCheck --check-prefix=NM %s

# The export directory, which opens .rdata ahead of the witness byte, has two
# functions and four names.
# EXPORTS:      Export Table:
# EXPORTS:      Ordinal      RVA  Name
# EXPORTS-NEXT:       1   0x20ce  __abis$SE709m-9Gmo$AAECAwQFBgcICQoLDA0ODw
# EXPORTS-NEXT:       2   0x1000  f
# EXPORTS-EMPTY:

# IMPLIB:      Name type: export as
# IMPLIB-NEXT: Export name: __abis$SE709m-9Gmo$AAECAwQFBgcICQoLDA0ODw
# IMPLIB-NEXT: Symbol: __imp___abis$SE709m-9Gmo$AAECAwQFBgcICQoLDA0ODw$a.dll
# IMPLIB:      Export name: __abiw$PwFGDH9_sEQ$EBESExQVFhcYGRobHB0eHw
# IMPLIB-NEXT: Symbol: __imp___abiw$PwFGDH9_sEQ$EBESExQVFhcYGRobHB0eHw$a.dll
# IMPLIB:      Export name: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw
# IMPLIB-NEXT: Symbol: __imp___abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw$a.dll

# NM-NOT: {{ }}__imp___abiw${{[^$]*}}${{[^$]*$}}
# NM-NOT: AAAAAAAAAAA

# An importer that imports every witness gets each name's own index in the
# export name table as its hint.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=HINTS %s
# HINTS:      Name: a.dll
# HINTS:      Symbol: __abis$SE709m-9Gmo$AAECAwQFBgcICQoLDA0ODw (0)
# HINTS-NEXT: Symbol: __abiw$PwFGDH9_sEQ$EBESExQVFhcYGRobHB0eHw (1)
# HINTS-NEXT: Symbol: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw (2)
# HINTS-NEXT: Symbol: f (3)

# A link without -import-slots exports no witnesses.
# RUN: lld-link -dll -noentry a.obj -out:plain.dll
# RUN: llvm-objdump -p plain.dll | FileCheck --check-prefix=PLAIN %s
# PLAIN-NOT: __abi

# The names are reserved.
# RUN: not lld-link -import-slots -dll -noentry a.obj \
# RUN:   '-export:__abiw$x=f' -out:reserved.dll 2>&1 \
# RUN:   | FileCheck --check-prefix=RESERVED %s
# RESERVED: error: cannot export f as __abiw$x: the name is reserved for ABI contract witnesses

# A row is a locator and a digest.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: not lld-link -import-slots -dll -noentry bad.obj -out:bad.dll 2>&1 \
# RUN:   | FileCheck --check-prefix=BAD %s
# BAD: error: bad.obj: malformed /abiprovide: directive: Z7E5CzMxeVE

#--- a.s
  .text
  .globl f
f:
  retq
  .section .drectve,"yn"
  .ascii " -export:f /abiprovide:Z7E5CzMxeVEAAECAwQFBgcICQoLDA0ODw,PwFGDH9_sEQEBESExQVFhcYGRobHB0eHw"
  .ascii " /abiprovidedata:SE709m-9GmoAAECAwQFBgcICQoLDA0ODw"

#--- unrelated.s
  .text
  .globl unrelated
unrelated:
  retq
  .section .drectve,"yn"
  .ascii " /abiprovide:AAAAAAAAAAAAAECAwQFBgcICQoLDA0ODw"

#--- main.s
  .text
  .globl main
main:
  .linkcontract main, __imp_f, type, "1T", "000102030405060708090a0b0c0d0e0f"
  .linkcontract main, __imp_f, type, "1U", "101112131415161718191a1b1c1d1e1f"
  .linkcontract main, __imp_f, data, "datum", "000102030405060708090a0b0c0d0e0f"
  callq *__imp_f(%rip)
  retq

#--- bad.s
  .section .drectve,"yn"
  .ascii " /abiprovide:Z7E5CzMxeVE"
