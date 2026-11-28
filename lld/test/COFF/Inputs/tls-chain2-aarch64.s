## The chains of tls-chain2-x86_64.s on ARM64.
        .text
        .globl  two$tp
        .p2align 2
two$tp:
        adrp    x8, __imp_x$tls
        ldr     x8, [x8, :fused_lo12:__imp_x$tls]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
        .site_fact 9, 0
        ldr     x20, [x8]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
        .site_fact 9, 0
        ldr     w13, [x8, #12]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        ldp     x10, x9, [x20]
        adrp    x11, __imp___tp_key
        ldr     x11, [x11, :fused_lo12:__imp___tp_key]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp___tp_key
        ldr     x21, [x11]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        add     x10, x28, x10
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 5
        cmp     x9, #0
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 2
        ldr     x12, [x10]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp___tp_key
        .site_fact 8, 20
        eor     x11, x21, x12, ror #17
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 2
        csel    x9, x11, x10, ne
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 12
        .site_fact 14, 28
        ldrb    w14, [x9, x13]
        cbz     w14, .Lcold
.Ldone:
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        ldp     x10, x9, [x20]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
        .site_fact 9, 0
        ldr     w8, [x8, #8]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        add     x10, x28, x10
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 5
        cmp     x9, #0
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 2
        ldr     x12, [x10]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp___tp_key
        .site_fact 8, 20
        eor     x11, x21, x12, ror #17
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 2
        csel    x9, x11, x10, ne
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 8
        .site_fact 14, 28
        ldr     w0, [x9, x8]
        ret
.Lcold:
        bl      init
        adrp    x8, __imp_x$tls
        ldr     x8, [x8, :fused_lo12:__imp_x$tls]
        b       .Ldone
init:
        ret
