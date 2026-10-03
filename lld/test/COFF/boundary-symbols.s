# REQUIRES: x86

## With -boundary-symbols, a referenced, undefined _etext or etext is the end
## of .text, _edata or edata the end of .data's initialized data, and _end or
## end the end of .data, its uninitialized tail included.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/nodata.s -filetype=obj -o %t.nodata.obj

# RUN: lld-link %t.main.obj -entry:main -boundary-symbols -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -h -t %t.exe | FileCheck %s
# CHECK:      0 .text 00000001
# CHECK-NEXT: 1 .data 00000058
# CHECK:      (sec  1){{.*}} 0x00000001 _etext
# CHECK:      (sec  1){{.*}} 0x00000001 etext
# CHECK:      (sec  2){{.*}} 0x00000038 _edata
# CHECK:      (sec  2){{.*}} 0x00000038 edata
# CHECK:      (sec  2){{.*}} 0x00000058 _end
# CHECK:      (sec  2){{.*}} 0x00000058 end

## They are not defined by default, or with -boundary-symbols:no.
# RUN: not lld-link %t.main.obj -entry:main -out:%t.off.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OFF
# RUN: not lld-link %t.main.obj -entry:main -boundary-symbols \
# RUN:   -boundary-symbols:no -out:%t.off.exe 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OFF
# OFF: undefined symbol: _etext

## A symbol whose section the image does not have is an error.
# RUN: not lld-link %t.nodata.obj -entry:main -boundary-symbols \
# RUN:   -out:%t.nodata.exe 2>&1 | FileCheck %s --check-prefix=NODATA
# NODATA: error: _end is referenced, but the image has no .data section

#--- main.s
        .globl main
        .text
main:
        retq

        .data
        .quad _etext
        .quad etext
        .quad _edata
        .quad edata
        .quad _end
        .quad end
        .quad 0

        .bss
        .zero 32

#--- nodata.s
        .globl main
        .text
main:
        retq

        .section .rdata,"dr"
        .quad _end
