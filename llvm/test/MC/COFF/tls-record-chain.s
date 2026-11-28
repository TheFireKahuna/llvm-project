// DEFINE: %{x64} = llvm-mc -triple x86_64-pc-windows-ntposix
// DEFINE: %{a64} = llvm-mc -triple aarch64-pc-windows-ntposix -defsym=A64=1
// RUN: %{x64} -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s --check-prefixes=X64
// RUN: not %{x64} -filetype=obj -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR --implicit-check-not=error:
// RUN: %if aarch64-registered-target %{ %{a64} -filetype=obj %s -o %t.a.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -r --coff-link-records %t.a.o | FileCheck %s --check-prefix=A64 %}

// A record chain's derived values: the place (1), the thread pointer plus it
// (2), the wide word and each step decoding it (3), the key (4) and on ARM64
// the form in a register of its own (5). The instruction that reads one, of
// any form, carries a FUSED_INDEX with its code, in its first register source
// where ViaFirst is set and in its second, or only, one otherwise, and may
// carry another marker, before or after; each marker states its own facts.
// The register holds what a reader of a derived value wrote, or for the place,
// the form and the key a word that an index or an access through the marker's
// pointer loaded; every reader of a value a reader wrote carries its marker,
// but an index of the variable's offset or its guard's, which reads the base.

// X64:      0x1F IMAGE_REL_LLVM_FUSED_ACCESS __imp___tp_key
// X64-NEXT: 0x1F IMAGE_REL_LLVM_FUSED_INDEX __imp_v$tls
// X64:      0x27 IMAGE_REL_LLVM_FUSED_INDEX __imp_v$tls
// X64-NEXT: 0x27 IMAGE_REL_LLVM_FUSED_INDEX __imp_v$tls
// X64:      Relocation: 3
// X64-NEXT: Fact: 8
// X64-NEXT: Value: 1
// X64:      Relocation: 7
// X64-NEXT: Fact: 8
// X64-NEXT: Value: 19
// X64:      Relocation: 9
// X64-NEXT: Fact: 8
// X64-NEXT: Value: 19
// X64:      Relocation: 10
// X64-NEXT: Fact: 8
// X64-NEXT: Value: 2

// A64:      0x2C IMAGE_REL_LLVM_FUSED_INDEX __imp___tp_key
// A64-NEXT: 0x2C IMAGE_REL_LLVM_FUSED_INDEX __imp_v$tls
// A64:      Relocation: 9
// A64-NEXT: Fact: 8
// A64-NEXT: Value: 5
// A64:      Relocation: 11
// A64-NEXT: Fact: 8
// A64-NEXT: Value: 20
// A64:      Relocation: 12
// A64-NEXT: Fact: 8
// A64-NEXT: Value: 3

.ifdef ERR
// ERR: error: fused index reads RDX, which no record chain of '__imp_v$tls' derived
  movq $1, %rdx
.Le0: .reloc .Le0, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 1
  leaq (%r14,%rdx), %rsi
// ERR: error: fused index of a derived value is not at an instruction that reads it
.Le1: .reloc .Le1, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 2
  retq
// Every reader of a derived value carries its marker, but an index of an
// offset that reads the base.
  movq __imp_v$tls@FUSED(%rip), %rax
.Le2: .reloc .Le2, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v$tls
  movq (%rax), %rcx
.Le3: .reloc .Le3, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 0
  movq (%rcx), %rdx
.Le4: .reloc .Le4, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 1
  leaq (%r14,%rdx), %rsi
// ERR: error: instruction reads RSI, which a record chain of '__imp_v$tls' derived, with no fused index of it
  movq %rsi, %rbx
.Le5: .reloc .Le5, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v$tls
  movl 8(%rax), %eax
.Le6: .reloc .Le6, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 8
  movl (%rsi,%rax), %eax
  retq
.else
.ifdef A64
  adrp x8, __imp_v$tls
  ldr x8, [x8, :fused_lo12:__imp_v$tls]
.Lt0: .reloc .Lt0, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v$tls
  .site_fact 9, 0
  ldr x9, [x8]
.Lt1: .reloc .Lt1, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v$tls
  .site_fact 9, 0
  ldr w8, [x8, #8]
.Lt2: .reloc .Lt2, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 0
  ldp x10, x9, [x9]
  adrp x12, __imp___tp_key
  ldr x12, [x12, :fused_lo12:__imp___tp_key]
.Lt3: .reloc .Lt3, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp___tp_key
  ldr x12, [x12]
.Lt4: .reloc .Lt4, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 1
  add x10, x28, x10
.Lt5: .reloc .Lt5, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 5
  cmp x9, #0
.Lt6: .reloc .Lt6, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 2
  ldr x11, [x10]
.Lt7: .reloc .Lt7, IMAGE_REL_LLVM_FUSED_INDEX, __imp___tp_key
  .site_fact 8, 20
.Lt8: .reloc .Lt8, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 3
  eor x11, x12, x11, ror #17
.Lt9: .reloc .Lt9, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 19
.Lt10: .reloc .Lt10, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 2
  csel x9, x11, x10, ne
.Lt11: .reloc .Lt11, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 8
  ldr w0, [x9, x8]
// The compare above wrote the zero register, which holds no derived value,
// and a write of a register's low half writes all of it.
  str wzr, [x1]
  mov w9, #1
  ldr w0, [x1, x9]
  ret
.else
  movq __imp_v$tls@FUSED(%rip), %rax
.Lt0: .reloc .Lt0, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v$tls
  .site_fact 9, 0
  movq (%rax), %rcx
.Lt1: .reloc .Lt1, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 0
  movq (%rcx), %rdx
.Lt2: .reloc .Lt2, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 1
  leaq (%r14,%rdx), %rsi
  movq __imp___tp_key@FUSED(%rip), %rdi
.Lt3: .reloc .Lt3, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 1
  rorxq $17, (%r14,%rdx), %rdx
.Lt5: .reloc .Lt5, IMAGE_REL_LLVM_FUSED_ACCESS, __imp___tp_key
.Lt4: .reloc .Lt4, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 19
  xorq (%rdi), %rdx
.Lt6: .reloc .Lt6, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 0
  cmpq $0, 8(%rcx)
.Lt7: .reloc .Lt7, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 19
.Lt8: .reloc .Lt8, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 2
  cmoveq %rsi, %rdx
.Lt9: .reloc .Lt9, IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v$tls
  .site_fact 9, 0
  movl 8(%rax), %eax
.Lt10: .reloc .Lt10, IMAGE_REL_LLVM_FUSED_INDEX, __imp_v$tls
  .site_fact 8, 8
  movl (%rdx,%rax), %eax
// A xor of a register from itself reads nothing of it, and a write of a
// register's low half writes all of it.
  xorl %edx, %edx
  movl $1, %esi
  addq %rsi, %rdx
  retq
.endif
.endif
