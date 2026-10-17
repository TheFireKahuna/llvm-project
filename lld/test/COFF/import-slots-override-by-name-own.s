# REQUIRES: x86
## Under -import-slots, a DLL's own default replaced by name has cells, as its
## address in the object that defines it is the program's one on ELF: the
## pointer its address-takes read, which no instruction bypasses, and the words
## of its static data that hold its address. Each holds the default until the
## DLL's fill writes the address the anchor's entry with the default's key
## names. The read-only ones are written within the fill's protection bracket,
## and the pointer draws no warning about a locally defined import.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium l.s -o l.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry l.obj crt.obj ntdll.lib -out:l.dll \
# RUN:   2>&1 | count 0
# RUN: llvm-objdump -d --print-imm-hex l.dll | FileCheck %s

# CHECK:      <take>:
# CHECK-NEXT: movq    {{.*}}(%rip), %rax # 0x[[#%x,POINTER:]]
## The writable word is written in place.
# CHECK:      movabsq $-0x4571ca1ea5a1ce2e, %rdx
# CHECK:      movq    0x8(%rax), %rax
# CHECK-NEXT: movq    %rax, {{.*}}(%rip) # 0x{{[0-9a-f]+}} <gw>
## The pointer and the read-only word, within the bracket.
# CHECK:      cmpl    $0x0, 0x8c(%r11)
# CHECK:      callq
# CHECK:      movabsq $-0x4571ca1ea5a1ce2e, %rdx
# CHECK:      movq    0x8(%rax), %rax
# CHECK-NEXT: movq    %rax, {{.*}}(%rip) # 0x[[#POINTER]]
# CHECK:      movabsq $-0x4571ca1ea5a1ce2e, %rdx
# CHECK:      movq    0x8(%rax), %rax
# CHECK-NEXT: movq    %rax, {{.*}}(%rip) # 0x{{[0-9a-f]+}} <gr>

#--- l.s
        .text
        .globl take
take:
        movq __imp_hook(%rip), %rax
        retq
        .weak hook
hook:
        xorl %eax, %eax
        retq
        .data
        .globl gw
gw:
        .quad hook
        .section .rdata,"dr"
        .globl gr
        .p2align 3
gr:
        .quad hook
        .section .drectve,"yni"
        .ascii " /OVERRIDABLEBYNAME:hook /EXPORT:hook /EXPORT:take"
        .ascii " /EXPORT:gw,DATA /EXPORT:gr,DATA"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .quad 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .quad 0
