# REQUIRES: x86

# Objects that each give a weak alias the same absolute value, as each object
# that takes the address of a declaration gives __kcfi_typeid_<name>, define
# one symbol. Different values are still a conflict.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym=VALUE=1234 typeid1.s -o typeid1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym=VALUE=1234 typeid2.s -o typeid2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym=VALUE=5678 typeid2.s -o typeid3.obj
# RUN: llvm-readobj --symbols typeid1.obj typeid2.obj | FileCheck %s --check-prefix=NAMES
# NAMES: Name: .weak.__kcfi_typeid_f.default.p1
# NAMES: Name: .weak.__kcfi_typeid_f.default.p2
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj

# RUN: lld-link -entry:main -out:same.exe main.obj typeid1.obj typeid2.obj
# RUN: llvm-objdump -s -j .data same.exe | FileCheck %s
# CHECK: d2040000

# RUN: not lld-link -entry:main -out:different.exe main.obj typeid1.obj \
# RUN:   typeid3.obj 2>&1 | FileCheck %s --check-prefix=DUP
# DUP: error: duplicate symbol: __kcfi_typeid_f

#--- typeid1.s
        .data
        .globl p1
p1:
        .quad 0
        .weak __kcfi_typeid_f
        .set __kcfi_typeid_f, VALUE

#--- typeid2.s
        .data
        .globl p2
p2:
        .quad 0
        .weak __kcfi_typeid_f
        .set __kcfi_typeid_f, VALUE

#--- main.s
        .text
        .globl main
main:
        retq
        .data
        .long __kcfi_typeid_f
