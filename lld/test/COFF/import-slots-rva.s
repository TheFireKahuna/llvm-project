# REQUIRES: aarch64, x86
## Under -import-slots, a 32-bit field in data cannot hold another image's
## address. Unwind and exception data name their handlers by RVAs that the
## system only calls, so an imported handler is served by its import thunk.
## Any other function address in such a field makes the image use the
## function's import thunk as its address everywhere, with a warning, so that
## its words in static data agree. A section-relative reference to an import
## is an error.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -def:a.def -out:a64.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc x86.s -o x86.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc arm64.s -o arm64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc secrel.s -o secrel.obj

# RUN: lld-link -import-slots -entry:main -subsystem:console x86.obj a.lib \
# RUN:   -out:x86.exe 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-imports --coff-basereloc x86.exe | \
# RUN:   FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-objdump -s -j .data x86.exe | FileCheck --check-prefix=X86 %s

# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   arm64.obj a64.lib -out:arm64.exe 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-imports arm64.exe | \
# RUN:   FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-objdump -s -j .data arm64.exe | FileCheck --check-prefix=ARM64 %s

# WARN:     warning: {{.*}}.obj: .data holds the address of f1, imported from a.dll, in a 32-bit field, so the image uses its import thunk as its address
# WARN-NOT: warning

## f2 stays in place; f1's word holds its thunk, with a base relocation.
# IMPORTS:      ImportAddressTableRVA: 0x3008
# IMPORTS-NEXT: Symbol: f2 (1)
# IMPORTS-NEXT: }

# X86:      140003000 10100040 01000000 86200000 00000000
# X86-NEXT: 140003010 10100000
# ARM64:      140003000 04100040 01000000 86200000 00000000
# ARM64-NEXT: 140003010 04100000

# RUN: not lld-link -import-slots -entry:main -subsystem:console x86.obj \
# RUN:   secrel.obj a.lib -out:err.exe 2>&1 | FileCheck --check-prefix=ERR %s
# ERR: error: secrel.obj: f3 is imported from a.dll, but .data refers to it with relocation type IMAGE_REL_AMD64_SECREL, which names a section of this image

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  f2
  f3

#--- x86.s
  .text
  .globl main
main:
  retq

  .data
  .quad f1
  .quad f2
  .rva f1

  .section .xdata,"dr"
  .rva f2

#--- arm64.s
  .text
  .globl main
main:
  ret

  .data
  .xword f1
  .xword f2
  .rva f1

  .section .xdata,"dr"
  .rva f2

#--- secrel.s
  .data
  .secrel32 f3
