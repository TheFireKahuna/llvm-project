# REQUIRES: x86

## With -boundary-symbols, a referenced, undefined _etext or etext is the end
## of .text, _edata or edata the end of .data's initialized data, and _end or
## end the end of .data, its uninitialized tail included.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/nodata.s -filetype=obj -o %t.nodata.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/weak.s -filetype=obj -o %t.weak.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/weakabsent.s -filetype=obj -o %t.weakabsent.obj

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

## Weak references are bounded as plain ones are.
# RUN: lld-link %t.weak.obj -entry:main -boundary-symbols -debug:symtab \
# RUN:   -out:%t.weak.exe
# RUN: llvm-objdump -h -t %t.weak.exe | FileCheck %s

## A weak reference to a symbol whose section the image does not have keeps
## its zero default.
# RUN: lld-link %t.weakabsent.obj -dll -noentry -boundary-symbols \
# RUN:   -out:%t.weakabsent.dll
# RUN: llvm-objdump -h -s %t.weakabsent.dll | FileCheck %s --check-prefix=WEAKABSENT
# WEAKABSENT-NOT: .text
# WEAKABSENT-NOT: .data
# WEAKABSENT:      Contents of section .rdata:
# WEAKABSENT-NEXT: 180001000 00000000 00000000 00000000 00000000
# WEAKABSENT-NEXT: 180001010 00000000 00000000

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

#--- weak.s
        .globl main
        .text
main:
        retq

        .weak _etext, etext, _edata, edata, _end, end
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

#--- weakabsent.s
        .weak _etext, _edata, _end
        .section .rdata,"dr"
        .quad _etext
        .quad _edata
        .quad _end
