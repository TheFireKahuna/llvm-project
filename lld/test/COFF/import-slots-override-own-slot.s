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
# RUN: llvm-objdump -d --print-imm-hex rt.dll > rt.txt
# RUN: llvm-objdump -s -j .rdata rt.dll >> rt.txt
# RUN: FileCheck --input-file=rt.txt %s
# CHECK:      <take>:
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x180002058
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]
## One read-only range over the read-only pointer at 0x2058, then the cells,
## {key, rva, kind}, each of slot 0, whose RVA is at 0x44 in the executable's
## header: the read-only pointer, bit 0 of its key set, and the writable word.
# CHECK:      [[#%x,LIST+0x30]] 58200000 08000000 01000000 44000000
# CHECK-NEXT: [[#%x,LIST+0x40]] 58200000 02000000 00000000 44000000
# CHECK-NEXT: [[#%x,LIST+0x50]] 00300000 02000000

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

        .def __llvm_fill
        .scl 2
        .type 32
        .endef
        .section .text,"xr",one_only,__llvm_fill
        .globl __llvm_fill
__llvm_fill:
        leaq __llvm_fill_list(%rip), %rax
        callq *__imp_NtProtectVirtualMemory(%rip)
        retq
