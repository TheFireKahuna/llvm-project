# REQUIRES: x86

# A call that reads an import pointer is folded onto the pointer by the
# compiler, which undoes the lift of that read out of a loop, and recorded in
# .impfuse$y: the function the call is in, the pointer it reads, the register
# the read left it in, and how far into the function the call is. The linker
# settles which of the two forms is wanted. Where the function is in this
# image the call becomes a direct one; where it really is in another image the
# register call is put back, because reading the table once is cheaper than
# reading it at every call. The call is seven bytes either way, so it returns
# to the address it returned to before and every entry that names an address
# in this image still names the right one.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %t.dir/main.s -o %t.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %t.dir/def.s -o %t.def.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:tick -implib:%t.lib.lib

# The function is in another image, so the register call comes back.
# RUN: lld-link -entry:main -subsystem:console -out:%t.imp.exe %t.obj %t.lib.lib
# RUN: llvm-objdump -d %t.imp.exe | FileCheck --check-prefix=IMPORTED %s

# The function is in this image, so the call is direct.
# RUN: lld-link -entry:main -subsystem:console -out:%t.loc.exe %t.obj %t.def.obj
# RUN: llvm-objdump -d %t.loc.exe | FileCheck --check-prefix=LOCAL %s

# The reads stay, because the calls through them are still calls through them.
# IMPORTED:      48 8b 1d {{.*}}movq {{.*}}%rbx
# IMPORTED:      4c 8b 2d {{.*}}movq {{.*}}%r13
# IMPORTED-NEXT: 2e 2e 2e 2e ff d3 {{.*}}callq *%rbx
# IMPORTED-NEXT: 2e 2e 2e 41 ff d5 {{.*}}callq *%r13
# IMPORTED-NEXT: 2e 2e 2e 2e ff e3 {{.*}}jmpq *%rbx
# Nothing else in the image is touched.
# IMPORTED-NOT:  callq *%

# The read is worth nothing once the calls are direct, and seven segment
# overrides in its place are decoded as part of the instruction after it, so
# nothing is left of it to execute and nothing has moved. The second read has
# nothing recorded to carry its bytes, so it becomes a no-operation, which
# costs a quarter of what materialising the address it held costs.
# LOCAL:      2e 2e 2e 2e 2e 2e 2e 31 c0 {{.*}}xorl %eax, %eax
# LOCAL-NEXT: 0f 1f 80 00 00 00 00 {{.*}}nopl
# LOCAL-NEXT: 67 e8 {{.*}}callq
# LOCAL-NEXT: 67 e8 {{.*}}callq
# LOCAL-NEXT: e9 {{.*}}jmp
# LOCAL-NOT:  callq *%


#--- main.s
        .text
        .globl  main
main:
# The read the compiler left in place, and the calls it folded. The calls are
# the six bytes any Windows compiler emits; what marks them is the record.
.Lload0:
        movq    __imp_tick(%rip), %rbx
.Lafter0:
        xorl    %eax, %eax
.Lend0:
.Lload1:
        movq    __imp_tick(%rip), %r13
.Lcall0:
        callq   *__imp_tick(%rip)
.Lcall1:
        callq   *__imp_tick(%rip)
.Ljump0:
        jmpq    *__imp_tick(%rip)

        .section        .impfuse$y,"dr"
        .p2align 2
        .symidx main
        .symidx __imp_tick
        .long   3               # rbx
        .long   .Lcall0-main
        .symidx main
        .symidx __imp_tick
        .long   13              # r13
        .long   .Lcall1-main
        .symidx main
        .symidx __imp_tick
        .long   3               # rbx
        .long   .Ljump0-main

# The read every one of those calls was folded out of: where it is, what lies
# between it and the instruction that could carry the overrides, how long that
# instruction is, and whether it is a transfer of control. The second read has
# no record, so it keeps its register, as 6.1 leaves it.
        .section        .impload$y,"dr"
        .p2align 2
        .symidx main
        .symidx __imp_tick
        .long   .Lload0-main
        .long   .Lafter0-.Lload0
        .long   .Lend0-.Lafter0
        .long   0

# A read with nothing recorded to carry its bytes becomes a no-operation
# rather than being left alone.
        .symidx main
        .symidx __imp_tick
        .long   .Lload1-main
        .long   0
        .long   0
        .long   0

#--- def.s
        .text
        .globl  tick
tick:
        retq

#--- lib.s
        .text
        .globl  tick
tick:
        retq
