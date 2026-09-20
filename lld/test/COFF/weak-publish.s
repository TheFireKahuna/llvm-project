# REQUIRES: x86

# A weak function definition is the implementation's fallback, and a strong
# definition elsewhere in the program supersedes it. The compiler records each
# one in .wkintp; naming __wkintp_start is what says an image's startup code
# binds them, and the linker then bounds the run and, for a program, writes
# the table those records are matched against: the 128-bit xxh3 of each
# exported name, sorted on the low half, with the export's RVA in a parallel
# array. A program is where a replacement makes itself reachable, so a library
# publishes nothing, and an image built by other tools carries no table.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %t.dir/main.s -o %t.main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %t.dir/plain.s -o %t.plain.obj

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.main.obj -export:zzz -export:aaa
# RUN: llvm-readobj --section-headers %t.exe | FileCheck --check-prefix=SECTION %s
# RUN: llvm-objdump -s -j .wkpub %t.exe | FileCheck --check-prefix=TABLE %s
# The bounds are the record, with nothing else between them.
# RUN: llvm-nm --numeric-sort %t.exe | FileCheck --check-prefix=BOUNDS %s

# An image whose startup code does not name the bounds takes no part.
# RUN: lld-link -entry:main -subsystem:console -out:%t.plain.exe %t.plain.obj -export:zzz -export:aaa
# RUN: llvm-readobj --section-headers %t.plain.exe | FileCheck --check-prefix=NONE %s

# A library does not interpose, so it publishes nothing.
# RUN: lld-link -dll -noentry -out:%t.dll %t.main.obj -export:zzz -export:aaa
# RUN: llvm-readobj --section-headers %t.dll | FileCheck --check-prefix=NONE %s

# Two exports: an 8-byte header, two 16-byte hashes and two 4-byte RVAs.
# SECTION:      Name: .wkpub
# SECTION-NEXT: VirtualSize: 0x30

# NONE-NOT: Name: .wkpub

# BOUNDS:      [[#%x,START:]] {{.}} __wkintp_start
# BOUNDS-NEXT: [[#%x,START+0x20]] {{.}} __wkintp_end

# TABLE:      Contents of section .wkpub:
# The count, then "zzz" before "aaa": the order is the hashes', not the
# exports'.
# TABLE-NEXT: {{[0-9a-f]+}} 02000000 00000000 bc89b20c 47cc3288
# TABLE-NEXT: {{[0-9a-f]+}} 28d3a5a9 447e4170 efc95d79 2832bae4
# "aaa" is at 0x1000 and "zzz" at 0x1010, so the parallel array of RVAs is
# not in order either.
# TABLE-NEXT: {{[0-9a-f]+}} 2e20c792 04b0a41b 10100000 00100000

#--- main.s
        .text
        .globl  aaa
        .p2align 4
aaa:
        retq

        .globl  zzz
        .p2align 4
zzz:
        retq

        .globl  main
main:
        movq    __wkintp_start(%rip), %rax
        movq    __wkintp_end(%rip), %rax
        xorl    %eax, %eax
        retq

# One record from the compiler.
        .section        .wkintp,"dr"
        .p2align 3
        .quad   1
        .quad   2
        .quad   0
        .quad   aaa

#--- plain.s
        .text
        .globl  aaa
        .p2align 4
aaa:
        retq

        .globl  zzz
        .p2align 4
zzz:
        retq

        .globl  main
main:
        xorl    %eax, %eax
        retq
