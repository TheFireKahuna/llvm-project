## Two chains of x around its guard's cold call, as the compiler emits them:
## they read the place word's address that one load gave and the key that
## one load gave, and the cold part loads the record's pointer again. Every
## chain of the record is stated, so that every one relaxes.
        .text
        .globl  two$tp
two$tp:
        movq    __imp_x$tls@FUSED(%rip), %rax
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movq    (%rax), %rbx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        movq    (%rbx), %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        leaq    (%r14,%rdx), %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        movq    (%r14,%rdx), %rdx
        movq    __imp___tp_key@FUSED(%rip), %rdi
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp___tp_key
        movq    (%rdi), %r12
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        rorq    $17, %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp___tp_key
        .site_fact 8, 4
        xorq    %r12, %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        cmpq    $0, 8(%rbx)
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 18
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        cmovneq %rdx, %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movl    12(%rax), %ecx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 12
        .site_fact 14, 14
        cmpb    $0, (%rsi,%rcx)
        je      .Lcold
.Ldone:
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        movq    (%rbx), %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        leaq    (%r14,%rdx), %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        movq    (%r14,%rdx), %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        rorq    $17, %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp___tp_key
        .site_fact 8, 4
        xorq    %r12, %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        cmpq    $0, 8(%rbx)
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 18
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        cmovneq %rdx, %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movl    8(%rax), %eax
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 8
        .site_fact 14, 14
        movl    (%rsi,%rax), %eax
        retq
.Lcold:
        callq   init
        movq    __imp_x$tls@FUSED(%rip), %rax
        jmp     .Ldone
init:
        retq
