# REQUIRES: x86
## Under -import-slots, a DLL's own default with a fixed slot has cells, as one
## replaced by name does: the pointer its address-takes read and the words of
## its static data that hold its address. The fill keys them by the slot that
## the default's definition records, the anchor's mask naming it replaced, so
## the DLL observes the program's replacement as every other image does. A DLL
## that does not take its default's address has no cell, and no fill.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium take.s -o take.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry rt.obj take.obj crt.obj ntdll.lib \
# RUN:   -out:rt.dll 2>&1 | count 0
# RUN: llvm-objdump -d --print-imm-hex rt.dll | FileCheck %s
# CHECK:      <take>:
# CHECK-NEXT: movq    {{.*}}(%rip), %rax # 0x[[#%x,POINTER:]]
## The writable word, keyed by slot 0.
# CHECK:      btl     $0x0, %r10d
# CHECK-NEXT: jae
# CHECK-NEXT: movl    0x44(%r11), %eax
# CHECK-NEXT: addq    %r11, %rax
# CHECK-NEXT: movq    %rax, {{.*}}(%rip) # 0x{{[0-9a-f]+}} <gw>
## The pointer, read-only, within the bracket.
# CHECK:      testl   $0x1, %r10d
# CHECK:      callq
# CHECK:      btl     $0x0, %r10d
# CHECK-NEXT: jae
# CHECK-NEXT: movl    0x44(%r11), %eax
# CHECK-NEXT: addq    %r11, %rax
# CHECK-NEXT: movq    %rax, {{.*}}(%rip) # 0x[[#POINTER]]

## Without an address-take there is no cell, no fill and no import of
## NtProtectVirtualMemory.
# RUN: lld-link -import-slots -dll -noentry rt.obj crt.obj ntdll.lib \
# RUN:   -out:plain.dll
# RUN: llvm-readobj --coff-imports plain.dll | FileCheck --check-prefix=PLAIN %s
# PLAIN-NOT: NtProtectVirtualMemory

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy"

#--- take.s
        .text
        .globl take
take:
        movq __imp__Znwy(%rip), %rax
        retq
        .data
        .globl gw
gw:
        .quad _Znwy
        .section .drectve,"yni"
        .ascii " /EXPORT:take /EXPORT:gw,DATA"

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
