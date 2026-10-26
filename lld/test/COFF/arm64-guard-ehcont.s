// REQUIRES: aarch64
// RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t.obj
// RUN: not lld-link %t.obj -guard:cf,ehcont -entry:main -out:%t.exe 2>&1 | FileCheck %s
// RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t-comdat.obj --defsym COMDAT=1
// RUN: lld-link %t-comdat.obj -guard:cf,ehcont -entry:main -out:%t.exe 2>&1 | FileCheck %s --check-prefix=COMDAT

// An object without EH continuation metadata whose ARM64 unwind data names a
// language handler is an error under /guard:ehcont, except SEH in a COMDAT.
// CHECK: error: {{.*}}.obj: object has no EH continuation metadata but needs it for __CxxFrameHandler3 under /guard:ehcont
// COMDAT: warning: {{.*}}comdat.obj: object has no EH continuation metadata; adding the __except targets of its SEH in COMDAT sections

.ifdef COMDAT
        .section .text,"xr",one_only,main
.endif
        .globl main
main:
        .seh_proc main
        .seh_endprologue
target:
        ret
.ifdef COMDAT
        .seh_handler __C_specific_handler, @unwind, @except
.else
        .seh_handler __CxxFrameHandler3, @unwind, @except
.endif
        .seh_handlerdata
        .word 1
        .rva main, target, main, target
.ifdef COMDAT
        .section .text,"xr",one_only,main
.else
        .text
.endif
        .seh_endproc

        .globl __CxxFrameHandler3
__CxxFrameHandler3:
__C_specific_handler:
        ret
