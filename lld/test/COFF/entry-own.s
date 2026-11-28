# REQUIRES: x86

## A Windows Itanium or NT-POSIX image starts at an entry of its own. A DLL
## whose link defines no _DllMainCRTStartup has no entry, though an import
## library names one, and an entry that resolves to an import is an error.
## Without such an object the import's thunk is the entry, as link.exe makes it.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix own.s -o own.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc dll.s -o msvc.obj
# RUN: llvm-ar rcs own.lib own.obj
# RUN: lld-link -machine:x64 -def:rt.def -out:rt.lib

# RUN: lld-link -dll -noimplib dll.obj rt.lib -out:none.dll
# RUN: llvm-readobj --file-headers --coff-imports none.dll \
# RUN:   | FileCheck --check-prefix=NONE --implicit-check-not='Import {' %s
# RUN: lld-link -dll -noimplib -unresolved-host-imports dll.obj -out:host.dll
# RUN: llvm-readobj --file-headers --coff-imports host.dll \
# RUN:   | FileCheck --check-prefix=NONE --implicit-check-not='Import {' %s
# NONE: AddressOfEntryPoint: 0x0

# RUN: lld-link -dll -noimplib dll.obj own.lib rt.lib -out:own.dll
# RUN: llvm-readobj --file-headers --coff-imports own.dll > own.txt
# RUN: llvm-objdump -d own.dll >> own.txt
# RUN: FileCheck --check-prefix=OWN --implicit-check-not='Import {' %s < own.txt
# OWN: AddressOfEntryPoint: 0x1004
# OWN: 180001004: b8 01 00 00 00 movl $0x1, %eax

# RUN: not lld-link -dll -noimplib -entry:_DllMainCRTStartup dll.obj rt.lib \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=ERR %s
# RUN: not lld-link -subsystem:console dll.obj rt.lib -out:bad.exe 2>&1 \
# RUN:   | FileCheck --check-prefix=ERR-EXE %s
# ERR: error: entry point _DllMainCRTStartup is an import; an image's entry is its own
# ERR-EXE: error: entry point mainCRTStartup is an import; an image's entry is its own

# RUN: lld-link -dll -noimplib msvc.obj rt.lib -out:msvc.dll
# RUN: llvm-readobj --file-headers --coff-imports msvc.dll > msvc.txt
# RUN: llvm-objdump -d msvc.dll >> msvc.txt
# RUN: FileCheck --check-prefix=MSVC %s < msvc.txt
# MSVC: AddressOfEntryPoint: 0x[[#%X,ENTRY:]]
# MSVC:   Symbol: _DllMainCRTStartup
# MSVC: [[#%x,ENTRY+0x180000000]]: ff 25 {{.*}} jmpq *

#--- dll.s
  .text
  .globl f
f:
  ret

#--- own.s
  .text
  .globl _DllMainCRTStartup
_DllMainCRTStartup:
  movl $1, %eax
  ret

#--- rt.def
LIBRARY rt.dll
EXPORTS
  _DllMainCRTStartup
  mainCRTStartup
