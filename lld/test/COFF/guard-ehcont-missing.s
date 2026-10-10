# REQUIRES: x86

# With /guard:ehcont, an object without EH continuation metadata whose unwind
# data names a language handler, or that references _local_unwind, has
# continuation targets that no table lists, and is an error. __GSHandlerCheck
# only checks the stack cookie and is accepted, as is an object with no
# handler, one with the metadata, and any object when no table is made.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc defs.s -o defs.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc inst.s -o inst.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc gs.s -o gs.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym=H=1 handler.s -o seh.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym=H=2 handler.s -o cxx.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym=H=3 handler.s -o gxx.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc localunwind.s -o localunwind.obj

# RUN: lld-link -guard:ehcont -dll -noentry defs.obj inst.obj plain.obj gs.obj \
# RUN:   -out:ok.dll 2>&1 | FileCheck %s --check-prefix=OK --allow-empty
# OK-NOT: error:

# RUN: lld-link -guard:cf -dll -noentry defs.obj seh.obj localunwind.obj \
# RUN:   -out:notable.dll 2>&1 | FileCheck %s --check-prefix=OK --allow-empty

# RUN: not lld-link -guard:ehcont -dll -noentry defs.obj seh.obj -out:seh.dll 2>&1 \
# RUN:   | FileCheck %s --check-prefix=SEH
# SEH: error: /guard:ehcont: seh.obj has no EH continuation metadata but its unwind data names exception handler __C_specific_handler

# RUN: not lld-link -guard:ehcont,nocf -dll -noentry defs.obj seh.obj \
# RUN:   -out:seh.dll 2>&1 | FileCheck %s --check-prefix=SEH

# RUN: not lld-link -guard:ehcont -dll -noentry defs.obj cxx.obj -out:cxx.dll 2>&1 \
# RUN:   | FileCheck %s --check-prefix=CXX
# CXX: error: /guard:ehcont: cxx.obj has no EH continuation metadata but its unwind data names exception handler __CxxFrameHandler4

# RUN: not lld-link -guard:ehcont -dll -noentry defs.obj gxx.obj -out:gxx.dll 2>&1 \
# RUN:   | FileCheck %s --check-prefix=GXX
# GXX: error: /guard:ehcont: gxx.obj has no EH continuation metadata but its unwind data names exception handler __gxx_personality_seh0

# RUN: not lld-link -guard:ehcont -dll -noentry defs.obj localunwind.obj -out:lu.dll 2>&1 \
# RUN:   | FileCheck %s --check-prefix=LU
# LU: error: /guard:ehcont: localunwind.obj has no EH continuation metadata but references _local_unwind

#--- defs.s
        .globl @feat.00
.set @feat.00, 0x4800
        .text
        .globl __C_specific_handler, __CxxFrameHandler4
        .globl __gxx_personality_seh0, __GSHandlerCheck, _local_unwind
__C_specific_handler:
__CxxFrameHandler4:
__gxx_personality_seh0:
__GSHandlerCheck:
_local_unwind:
        retq

#--- inst.s
        .globl @feat.00
.set @feat.00, 0x4800
        .text
        .globl inst
        .def inst; .scl 2; .type 32; .endef
        .seh_proc inst
inst:
        .seh_handler __C_specific_handler, @except
        subq $40, %rsp
        .seh_stackalloc 40
        .seh_endprologue
        addq $40, %rsp
        retq
        .seh_endproc
        .section .gehcont$y,"dr"

#--- plain.s
        .text
        .globl plain
        .def plain; .scl 2; .type 32; .endef
        .seh_proc plain
plain:
        subq $40, %rsp
        .seh_stackalloc 40
        .seh_endprologue
        addq $40, %rsp
        retq
        .seh_endproc

#--- gs.s
        .text
        .globl gs
        .def gs; .scl 2; .type 32; .endef
        .seh_proc gs
gs:
        .seh_handler __GSHandlerCheck, @except
        subq $40, %rsp
        .seh_stackalloc 40
        .seh_endprologue
        addq $40, %rsp
        retq
        .seh_endproc

#--- handler.s
        .text
        .globl f
        .def f; .scl 2; .type 32; .endef
        .seh_proc f
f:
.if H == 1
        .seh_handler __C_specific_handler, @except
.elseif H == 2
        .seh_handler __CxxFrameHandler4, @except, @unwind
.else
        .seh_handler __gxx_personality_seh0, @unwind, @except
.endif
        subq $40, %rsp
        .seh_stackalloc 40
        .seh_endprologue
        addq $40, %rsp
        retq
        .seh_endproc

#--- localunwind.s
        .text
        .globl g
g:
        jmp _local_unwind
