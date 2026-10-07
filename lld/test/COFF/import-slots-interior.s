# REQUIRES: x86
## Under -import-slots, a word of static data that holds an address inside an
## imported object takes the name that the object's DLL exports for that
## address, X$soK for a subobject of a variable and X$apK for a vtable's
## address point, which the loader writes as it writes any import. The import
## library member offering the name is loaded only for such a word, a name that
## another DLL exports does not count, and an import whose words all take
## interior names keeps no entry of its own.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -def:other.def -out:other.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium words.s -o words.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium base.s -o base.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium other.s -o other.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium func.s -o func.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll words.obj lib.lib
# RUN: llvm-readobj --coff-imports out.dll | FileCheck --check-prefix=NAMES %s
# RUN: lld-link -dll -noentry -import-slots -opt:noref -out:noref.dll \
# RUN:   words.obj lib.lib
# RUN: llvm-readobj --coff-imports noref.dll | FileCheck --check-prefix=NAMES %s

# NAMES:      Name: lib.dll
# NAMES-NEXT: ImportLookupTableRVA:
# NAMES-NEXT: ImportAddressTableRVA:
# NAMES-NEXT: Symbol: obj$so8 ({{[0-9]+}})
# NAMES-NEXT: Symbol: _ZTV1D$ap16 ({{[0-9]+}})
# NAMES-NEXT: }
# NAMES-NOT:  Symbol:

## A word that holds the object itself keeps its import.
# RUN: lld-link -dll -noentry -import-slots -out:base.dll words.obj base.obj \
# RUN:   lib.lib
# RUN: llvm-readobj --coff-imports base.dll | FileCheck --check-prefix=BASE %s

# BASE:     Symbol: obj$so8 ({{[0-9]+}})
# BASE:     Symbol: _ZTV1D$ap16 ({{[0-9]+}})
# BASE:     Symbol: obj ({{[0-9]+}})
# BASE-NOT: Symbol:

## other.dll's name for an address inside lib.dll's obj is not obj's.
# RUN: not lld-link -dll -noentry -import-slots -out:other.dll other.obj \
# RUN:   lib.lib other.lib 2>&1 | FileCheck --check-prefix=OTHER %s

# OTHER: error: other.obj: .rdata holds the address of obj plus 24, imported from lib.dll, which exports no name for it

## No address inside an imported function is imported.
# RUN: not lld-link -dll -noentry -import-slots -out:func.dll func.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=FUNC %s

# FUNC: error: func.obj: .rdata holds the address of func plus 4, imported from lib.dll, but no address inside a function is imported

#--- lib.def
LIBRARY lib.dll
EXPORTS
  obj DATA
  obj$so8 DATA
  obj$so16 DATA
  _ZTV1D DATA
  _ZTV1D$ap16 DATA
  func

#--- other.def
LIBRARY other.dll
EXPORTS
  obj$so24 DATA

#--- words.s
  .section .rdata,"dr"
  .p2align 3
  .quad obj+8
  .quad _ZTV1D+16

#--- base.s
  .section .rdata,"dr"
  .p2align 3
  .quad obj

#--- other.s
  .section .rdata,"dr"
  .p2align 3
  .quad obj+24

#--- func.s
  .section .rdata,"dr"
  .p2align 3
  .quad func+4
