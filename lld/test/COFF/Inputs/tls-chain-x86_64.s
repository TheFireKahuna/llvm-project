## A record chain of x in the owned model, as the compiler emits it: the
## record, the place word, the base, the wide word decoded with the key, the
## select on the form, then x's guard and x through the selected base. Each
## reader of a value the chain derives is a FUSED_INDEX, its fact stating the
## value, and the cmov's destination is the direct arm.
        .text
        .globl  use$tp
use$tp:
        movq    __imp_x$tls@FUSED(%rip), %rax
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movq    (%rax), %rcx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        movq    (%rcx), %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        leaq    (%r14,%rdx), %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        movq    (%r14,%rdx), %rdx
        movq    __imp___tp_key@FUSED(%rip), %rdi
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        rorq    $17, %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp___tp_key
        xorq    (%rdi), %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        cmpq    $0, 8(%rcx)
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
        cmpb    $0, (%rsi,%rcx)
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movl    8(%rax), %eax
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 8
        movl    (%rsi,%rax), %eax
        retq
