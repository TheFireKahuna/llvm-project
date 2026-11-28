## _tls_used and the symbols it names, for images with thread-local data: the
## linker defines _tls_start and _tls_end at the loader's template.
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
