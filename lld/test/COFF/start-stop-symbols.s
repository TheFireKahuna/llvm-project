# REQUIRES: x86

## In MinGW links, a referenced, undefined __start_X or __stop_X, where X is a
## C identifier, bounds the sections named X or X$*, and _etext, _edata and
## _end, unprefixed too, end .text, .data's initialized data and .data.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple=x86_64-windows-gnu main.s -filetype=obj -o main.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu weak.s -filetype=obj -o weak.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu gc.s -filetype=obj -o gc.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu errors.s -filetype=obj -o errors.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu split.s -filetype=obj -o split.obj
# RUN: llvm-mc -triple=x86_64-windows-gnu nodata.s -filetype=obj -o nodata.obj

# RUN: lld-link -lldmingw -entry:main -debug:symtab -out:main.exe main.obj
# RUN: llvm-objdump -h -s -t main.exe | FileCheck %s
# CHECK:      0 .text 00000001
# CHECK:      2 .data 00000060
# CHECK:      3 myset 00000018
# CHECK:      (sec  4){{.*}} 0x00000000 __start_myset
# CHECK-NEXT: (sec  4){{.*}} 0x00000018 __stop_myset
# CHECK-NEXT: (sec  1){{.*}} 0x00000001 _etext
# CHECK-NEXT: (sec  1){{.*}} 0x00000001 etext
# CHECK-NEXT: (sec  3){{.*}} 0x00000040 _edata
# CHECK-NEXT: (sec  3){{.*}} 0x00000040 edata
# CHECK-NEXT: (sec  3){{.*}} 0x00000060 _end
# CHECK-NEXT: (sec  3){{.*}} 0x00000060 end
# CHECK:      Contents of section myset:
# CHECK-NEXT: 140004000 00000000 00000000 01000000 00000000
# CHECK-NEXT: 140004010 02000000 00000000

## MSVC links define none of them.
# RUN: not lld-link -entry:main -out:msvc.exe main.obj 2>&1 \
# RUN:   | FileCheck %s --check-prefix=MSVC
# MSVC-DAG: undefined symbol: __start_myset
# MSVC-DAG: undefined symbol: _etext

## A link with an object that LLVM made for Windows Itanium or NT-POSIX
## defines them as a MinGW link does.
# RUN: llvm-mc -triple=x86_64-windows-itanium main.s -filetype=obj \
# RUN:   -o itanium.obj
# RUN: lld-link -entry:main -out:itanium.exe itanium.obj
# RUN: llvm-objdump -s -j myset itanium.exe \
# RUN:   | FileCheck %s --check-prefix=ITANIUM
# ITANIUM:      Contents of section myset:
# ITANIUM-NEXT: 00000000 00000000 01000000 00000000

## A weak reference is bounded or defined as a plain one is, whatever its
## default, and keeps its zero default with nothing to bound or no such section.
# RUN: lld-link -lldmingw -entry:main -out:weak.exe weak.obj
# RUN: llvm-objdump -s -j .data weak.exe | FileCheck %s --check-prefix=WEAK
# WEAK:      140003000 00400040 01000000 00000000 00000000
# WEAK-NEXT: 140003010 20300040 01000000 08400040 01000000

## A reference to the bounds keeps no section of the run, except of a run
## named __libc_*, and ICF folds no bounded section. A run whose sections are
## all collected has equal bounds.
# RUN: lld-link -lldmingw -entry:main -opt:ref,icf -out:gc.exe gc.obj
# RUN: llvm-objdump -s gc.exe | FileCheck %s --check-prefix=GC
# GC-NOT:  section myset:
# GC:      Contents of section .data:
# GC-NEXT: 140003000 00500040 01000000 00000040 01000000
# GC-NEXT: 140003010 00000040 01000000 00400040 01000000
# GC:      Contents of section __libc_a:
# GC-NEXT: 140004000 00000000 00000000 01000000 00000000
# GC:      Contents of section fns:
# GC-NEXT: 140005000 b8010000 00c3b801 000000c3

# RUN: not lld-link -lldmingw -entry:main -out:errors.exe errors.obj 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ERR
# ERR: error: __start_defined cannot be defined by the linker: __stop_defined is defined by an input
# ERR: error: undefined symbol: __start_absent
# RUN: not lld-link -lldmingw -entry:main -out:split.exe split.obj 2>&1 \
# RUN:   | FileCheck %s --check-prefix=SPLIT
# SPLIT: error: section split is split across output sections and cannot be bounded by __start_split and __stop_split
# RUN: not lld-link -lldmingw -dll -noentry -out:nodata.dll nodata.obj 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NODATA
# NODATA: error: _etext is referenced, but the image has no .text section
# NODATA-NOT: _end

#--- main.s
  .globl main
main:
  retq

  .data
  .quad __start_myset
  .quad __stop_myset
  .quad _etext
  .quad etext
  .quad _edata
  .quad edata
  .quad _end
  .quad end
  .bss
  .zero 32

  .section myset$b,"dr"
  .quad 2
  .section myset,"dr"
  .quad 0
  .section myset$a,"dr"
  .quad 1

#--- weak.s
  .globl main
main:
  retq

  .weak __start_myset, __start_absent, _edata, __stop_myset
  .set __stop_myset, main
  .data
  .quad __start_myset
  .quad __start_absent
  .quad _edata
  .quad __stop_myset
  .section myset,"dr"
  .quad 1

#--- gc.s
  .globl main
main:
  callq f1
  callq f2
  retq

  .data
  .quad __start_fns
  .quad __start_myset
  .quad __stop_myset
  .quad __start___libc_a

  .section fns,"xr",discard,f1
  .globl f1
f1:
  movl $1, %eax
  retq
  .section fns,"xr",discard,f2
  .globl f2
f2:
  movl $1, %eax
  retq

  .section myset$a,"dr",discard,entry1
  .globl entry1
entry1:
  .quad 1

  .section __libc_a,"dr"
  .quad 0
  .section __libc_a$a,"dr",discard,entry2
  .globl entry2
entry2:
  .quad 1

#--- split.s
  .globl main
main:
  retq

  .data
  .quad __start_split
  .quad __stop_split
  .section split,"dr"
  .quad 0
  .section split$a,"dw"
  .quad 1

#--- errors.s
  .globl main
main:
  retq

  .data
  .quad __start_defined
  .quad __start_absent
  .section defined,"dr"
  .globl __stop_defined
  .quad 0
__stop_defined:

#--- nodata.s
  .weak _end
  .section .rdata,"dr"
  .quad _etext
  .quad _end
