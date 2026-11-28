## The chain of tls-chain-x86_64.s on ARM64: the place and the form in one
## ldp, the key through the importer's import, csel of the two arms.
        .text
        .globl  use$tp
        .p2align 2
use$tp:
        adrp    x8, __imp_x$tls
        ldr     x8, [x8, :fused_lo12:__imp_x$tls]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
        .site_fact 9, 0
        ldr     x9, [x8]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
        .site_fact 9, 0
        ldr     w13, [x8, #12]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
        .site_fact 9, 0
        ldr     w8, [x8, #8]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        ldp     x10, x9, [x9]
        adrp    x11, __imp___tp_key
        ldr     x11, [x11, :fused_lo12:__imp___tp_key]
        .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp___tp_key
        ldr     x11, [x11]
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
        eor     x11, x11, x12, ror #17
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 2
        csel    x9, x11, x10, ne
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 12
        ldrb    w14, [x9, x13]
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 8
        ldr     w0, [x9, x8]
        ret
