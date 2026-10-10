// REQUIRES: aarch64

// ARM64 unwind data: with /guard:ehcont, an object without EH continuation
// metadata whose unwind data names a language handler is an error, unless the
// handler is __GSHandlerCheck.

// RUN: split-file %s %t.dir && cd %t.dir
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows defs.s -o defs.obj
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows -defsym=H=0 handler.s -o gs.obj
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows -defsym=H=1 handler.s -o seh.obj
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows -defsym=H=2 handler.s -o gxx.obj
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows -defsym=H=1 -defsym=E=1 handler.s -o seh2.obj
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows ext.s -o ext.obj

// RUN: lld-link -machine:arm64 -guard:ehcont -dll -noentry defs.obj gs.obj \
// RUN:   -out:ok.dll 2>&1 | FileCheck %s --check-prefix=OK --allow-empty
// OK-NOT: error:
// RUN: lld-link -machine:arm64 -guard:cf -dll -noentry defs.obj seh.obj \
// RUN:   -out:notable.dll 2>&1 | FileCheck %s --check-prefix=OK --allow-empty

// RUN: not lld-link -machine:arm64 -guard:ehcont -dll -noentry defs.obj seh.obj \
// RUN:   -out:seh.dll 2>&1 | FileCheck %s --check-prefix=SEH
// RUN: not lld-link -machine:arm64 -guard:ehcont -dll -noentry defs.obj seh2.obj \
// RUN:   -out:seh2.dll 2>&1 | FileCheck %s --check-prefix=SEH
// SEH: error: /guard:ehcont: seh{{2?}}.obj has no EH continuation metadata but its unwind data names exception handler __C_specific_handler

// A record whose epilog count and code words are both zero has an extended
// header word holding them.
// RUN: not lld-link -machine:arm64 -guard:ehcont -dll -noentry defs.obj ext.obj \
// RUN:   -out:ext.dll 2>&1 | FileCheck %s --check-prefix=EXT
// EXT: error: /guard:ehcont: ext.obj has no EH continuation metadata but its unwind data names exception handler __C_specific_handler

// RUN: not lld-link -machine:arm64 -guard:ehcont -dll -noentry defs.obj gxx.obj \
// RUN:   -out:gxx.dll 2>&1 | FileCheck %s --check-prefix=GXX
// GXX: error: /guard:ehcont: gxx.obj has no EH continuation metadata but its unwind data names exception handler __gxx_personality_seh0

#--- defs.s
        .globl "@feat.00"
.set "@feat.00", 0x4800
        .text
        .globl __C_specific_handler, __gxx_personality_seh0, __GSHandlerCheck
__C_specific_handler:
__gxx_personality_seh0:
__GSHandlerCheck:
        ret

#--- handler.s
        .text
        .globl f
        .def f; .scl 2; .type 32; .endef
        .seh_proc f
f:
.if H == 0
        .seh_handler __GSHandlerCheck, @except
.elseif H == 1
        .seh_handler __C_specific_handler, @except
.else
        .seh_handler __gxx_personality_seh0, @unwind, @except
.endif
        stp x29, x30, [sp, #-16]!
        .seh_save_fplr_x 16
        .seh_endprologue
        bl g
.ifdef E
        // A second epilog keeps the scopes out of the header.
        cbz x0, 1f
        .seh_startepilogue
        ldp x29, x30, [sp], #16
        .seh_save_fplr_x 16
        .seh_endepilogue
        ret
1:
.endif
        .seh_startepilogue
        ldp x29, x30, [sp], #16
        .seh_save_fplr_x 16
        .seh_endepilogue
        ret
        .seh_endproc
g:
        ret

#--- ext.s
        .text
        .globl f
f:
        ret

        .section .xdata,"dr"
        .p2align 2
ext:
        .long 0x00100001            // X, no counts in the header, 1 word long
        .long 0x00010001            // one code word, one epilog scope
        .long 0x00000000            // the epilog scope
        .byte 0xe4, 0xe3, 0xe3, 0xe3 // end, nop, nop, nop
        .rva __C_specific_handler
        .long 0                     // handler data

        .section .pdata,"dr"
        .p2align 2
        .rva f
        .rva ext
