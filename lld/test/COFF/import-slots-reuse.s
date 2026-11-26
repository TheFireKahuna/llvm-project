# REQUIRES: x86, aarch64

## An import whose every pointer use reads all of it takes no IAT entry where an
## in-place slot (read-only, aligned, unnamed by /merge or /section) can serve
## as its pointer; a DLL left with no entry has its first run as its table.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry a.lib b.lib c.lib \
# DEFINE:   data.obj code.obj -include:main -include:__imp_g1 \
# DEFINE:   -call-graph-profile-sort:none
# DEFINE: %{run} = lld-link -machine:%{a} -def:a.def -out:a.lib && \
# DEFINE:   lld-link -machine:%{a} -def:b.def -out:b.lib && \
# DEFINE:   lld-link -machine:%{a} -def:c.def -out:c.lib && \
# DEFINE:   %{mc} data.s -o data.obj && %{mc} %{a}.s -o code.obj && \
# DEFINE:   %{link} -out:reuse.dll -map:reuse.map && \
# DEFINE:   llvm-readobj --coff-imports reuse.dll > reuse.txt && \
# DEFINE:   cat reuse.map >> reuse.txt && \
# DEFINE:   llvm-objdump -d reuse.dll >> reuse.txt && \
# DEFINE:   FileCheck --check-prefixes=CHECK,CODE-%{a} %s < reuse.txt && \
# DEFINE:   %{link} -out:named.dll -map:named.map -section:.rdata,r && \
# DEFINE:   FileCheck --check-prefix=NAMED %s < named.map

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## a.dll's table keeps the root and the data word's, lea's, misaligned slot's,
## slotless and code-read writable slot's imports.
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#%X,IAT:]]
# CHECK-NEXT: Symbol: g1 (
# CHECK-NEXT: Symbol: k1 (
# CHECK-NEXT: Symbol: l1 (
# CHECK-NEXT: Symbol: m1 (
# CHECK-NEXT: Symbol: n1 (
# CHECK-NEXT: Symbol: w1 (
# CHECK-NEXT: }
## b.dll has no table: its run is its descriptor, before c.dll's.
# CHECK:      Name: b.dll
# CHECK:      ImportAddressTableRVA: 0x3020
# CHECK-NEXT: Symbol: only (
# CHECK-NEXT: }
# CHECK:      Name: c.dll
# CHECK:      Name: a.dll
# CHECK-NOT:  Name: b.dll
## Pointers that left the table are slots after the tables: read-only for r1
## and t1 (whose thunk reads it), u1's writable one, which nothing reads.
# CHECK:      __imp_r1 {{0*}}[[#%x,IAT+0x180000048]]
# CHECK-NEXT: __imp_t1 {{0*}}[[#%x,IAT+0x180000068]]
# CHECK-NEXT: __imp_j1 {{0*}}[[#%x,IAT+0x1800000a8]]
# CHECK-NEXT: __imp_f1 {{0*}}[[#%x,IAT+0x1800000b8]]
# CHECK-NEXT: __imp_u1 {{0*}}180003000
# CHECK-NEXT: __imp_only {{0*}}180003020

# CODE-x64:      ff 15 {{.*}} callq *{{.*}}(%rip) {{.*}}# 0x[[#%x,IAT+0x180000048]]
# CODE-x64-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,IAT+0x180000048]]
# CODE-x64-NEXT: callq *{{.*}}(%rip) {{.*}}# 0x[[#%x,IAT+0x180000028]]
# CODE-x64:      48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,IAT+0x1800000b8]]
# CODE-x64:      ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}}# 0x[[#%x,IAT+0x1800000a8]]
# CODE-x64:      ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}}# 0x[[#%x,IAT+0x180000068]]

# CODE-arm64:      ldr x8, 0x[[#%x,IAT+0x180000048]]
# CODE-arm64:      ldr x8, 0x[[#%x,IAT+0x180000028]]
# CODE-arm64:      ldr x9, 0x[[#%x,IAT+0x1800000b8]]
# CODE-arm64:      ldr x16, 0x[[#%x,IAT+0x1800000a8]]
# CODE-arm64-NEXT: br x16
# CODE-arm64:      ldr x16, 0x[[#%x,IAT+0x180000068]]
# CODE-arm64-NEXT: br x16

## Slots in a section /section names serve no reader: r1 and t1 keep entries.
# NAMED:      __imp_n1 {{0*}}[[#%x,N1:]]
# NAMED-NEXT: __imp_r1 {{0*}}[[#N1+8]]
# NAMED-NEXT: __imp_t1 {{0*}}[[#N1+16]]

#--- a.def
LIBRARY a.dll
EXPORTS
  f1 g1 j1 k1 l1 m1 n1 r1 t1 u1 w1

#--- b.def
LIBRARY b.dll
EXPORTS
only

#--- c.def
LIBRARY c.dll
EXPORTS
c1

#--- data.s
.section .rdata,"dr"
.p2align 3
  .quad r1
  .quad 0
  .quad k1
  .quad 0
  .quad t1
  .quad 0
  .quad g1
  .quad 0
  .quad l1
  .quad 0
  .long 0
  .quad m1
  .long 0
  .quad j1
  .quad 0
  .quad f1
  .quad 0
.data
.p2align 3
  .quad u1
  .quad 0
  .quad w1
  .quad 0
  .quad only
  .quad __imp_k1
  .quad t1

#--- x64.s
.text
.globl main
main:
  callq *__imp_r1(%rip)
  movq __imp_r1(%rip), %rax
  callq *__imp_w1(%rip)
  callq t1
  movq __imp_m1(%rip), %rax
  callq *__imp_n1(%rip)
  leaq __imp_l1(%rip), %rax
  callq *__imp_c1(%rip)
  movq __imp_f1@FUSED(%rip), %rcx
  ret
  jmpq *__imp_j1(%rip)

#--- arm64.s
.text
.globl main
main:
  adrp x8, __imp_r1
  ldr x8, [x8, :lo12:__imp_r1]
  blr x8
  adrp x8, __imp_w1
  ldr x8, [x8, :lo12:__imp_w1]
  blr x8
  bl t1
  adrp x8, __imp_m1
  ldr x8, [x8, :lo12:__imp_m1]
  adrp x8, __imp_n1
  ldr x8, [x8, :lo12:__imp_n1]
  adrp x0, __imp_l1
  add x0, x0, :lo12:__imp_l1
  adrp x8, __imp_c1
  ldr x8, [x8, :lo12:__imp_c1]
  adrp x9, __imp_f1
  ldr x9, [x9, :fused_lo12:__imp_f1]
  ret
  adrp x16, __imp_j1
  ldr x16, [x16, :lo12:__imp_j1]
  br x16
