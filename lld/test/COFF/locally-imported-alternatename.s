# REQUIRES: x86

## Under -import-slots, an undefined __imp_X binds to the definition that
## /alternatename gives X, as a direct reference to X would, though nothing
## else references X. When the alternate is an import, __imp_X is the
## import's pointer. An alternate whose target nothing defines leaves __imp_X
## undefined.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc start.s -o start.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc impl.s -o impl.obj
# RUN: llvm-lib start.obj impl.obj -out:rt.lib
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:main.exe \
# RUN:   main.obj rt.lib 2>&1 | FileCheck %s

# CHECK: warning: main.obj: locally defined symbol imported: x_impl (defined in rt.lib(impl.obj)) [LNK4217]

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main-dll.s -o main-dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc impl.s -o dll.obj
# RUN: lld-link -dll -noentry -out:impl.dll dll.obj -export:x_impl \
# RUN:   -implib:impl.lib
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:dll.exe \
# RUN:   main-dll.obj start.obj impl.lib -map:dll.map 2>&1 \
# RUN:   | count 0
# RUN: llvm-readobj --coff-imports dll.exe | FileCheck --check-prefix=IMPORT %s
# RUN: FileCheck --check-prefix=MAP %s < dll.map

# IMPORT:      Name: impl.dll
# IMPORT:        Symbol: x_impl

## No thunk and no local pointer: __imp_x is x_impl's import address table
## entry.
# MAP-NOT:  0001:{{.*}} x_impl
# MAP:      __imp_x [[IAT:[0-9a-f]+]] impl:impl.dll
# MAP-NEXT: __imp_x_impl [[IAT]] impl:impl.dll

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main-missing.s \
# RUN:   -o main-missing.obj
# RUN: not lld-link -import-slots -entry:main -subsystem:console \
# RUN:   -out:missing.exe main-missing.obj rt.lib 2>&1 \
# RUN:   | FileCheck --check-prefix=MISSING %s

# MISSING:     error: undefined symbol: __declspec(dllimport) y
# MISSING-NOT: error: undefined symbol: y

#--- main.s
.text
.globl main
main:
  call *__imp_x(%rip)
  call start
  ret

#--- main-dll.s
.text
.globl main
main:
  call *__imp_x(%rip)
  ret

#--- main-missing.s
.text
.globl main
main:
  call *__imp_y(%rip)
  call start
  ret

## The member that the image loads anyway names the alternates.
#--- start.s
.text
.globl start
start:
  ret
.section .drectve,"yn"
.ascii " /alternatename:x=x_impl /alternatename:y=y_impl"

#--- impl.s
.text
.globl x_impl
x_impl:
  ret
