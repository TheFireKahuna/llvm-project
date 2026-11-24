## _tls_used and the symbols it names, for images with thread-local data.
        .section .tls,"dw"
        .globl  _tls_start
_tls_start:
        .quad   0
        .section .tls$ZZZ,"dw"
        .globl  _tls_end
_tls_end:
        .quad   0
        .data
        .globl  _tls_index
_tls_index:
        .long   0
        .section .rdata,"dr"
        .globl  _tls_used
_tls_used:
        .quad   _tls_start
        .quad   _tls_end
        .quad   _tls_index
        .quad   0
        .long   0
        .long   0
