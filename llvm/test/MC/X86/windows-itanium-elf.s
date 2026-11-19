# An ELF object for Windows Itanium or NT-POSIX has ELF's relocations.
# RUN: llvm-mc -triple=x86_64-unknown-windows-itanium-elf -filetype=obj %s -o - \
# RUN:   | llvm-readobj -r - | FileCheck %s
# RUN: llvm-mc -triple=x86_64-pc-windows-ntposix-elf -filetype=obj %s -o - \
# RUN:   | llvm-readobj -r - | FileCheck %s
# CHECK: R_X86_64_PLT32 f
jmp 1f
1: call f
