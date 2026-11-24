// DEFINE: %{x64} = llvm-mc -triple x86_64-unknown-windows-itanium
// DEFINE: %{a64} = llvm-mc -triple aarch64-unknown-windows-itanium -defsym=A64=1
// RUN: %{x64} -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s --check-prefixes=FACTS,X64
// RUN: %{x64} %s | %{x64} -filetype=obj -o %t.s.o
// RUN: llvm-readobj -r --coff-link-records %t.s.o | FileCheck %s --check-prefixes=FACTS,X64
// RUN: not %{x64} -filetype=obj -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=X64-ERR --implicit-check-not=error:
// RUN: not %{x64} -filetype=obj -defsym=NONE=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=NONE
// RUN: %if aarch64-registered-target %{ %{a64} -filetype=obj %s -o %t.a.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -r --coff-link-records %t.a.o | FileCheck %s --check-prefixes=FACTS,A64 %}
// RUN: %if aarch64-registered-target %{ not %{a64} -filetype=obj -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=A64-ERR --implicit-check-not=error: %}

// Code reaches a thread-local variable that may be another image's through
// its record, whose pointer it loads. A marked access loads a word of the
// record, and a FUSED_INDEX marks an instruction that reads such a word: the
// address of the TLS index as an access through it, and an offset as one of
// two registers that it adds. .site_fact states a fact of the marker before
// it, which the SiteFacts group of the link records keys by the index of the
// marker's relocation: 8, the word and whether its register is the first of
// the two, and 9, that indexes alone read the word that an access loads.
// A fact that no marker precedes is of the relocation that follows it.

.ifdef NONE
// NONE: error: site fact 7 is of no relocation
  .site_fact 7, 0
  retq
.else

.ifndef ERR
// X64:      0x3 IMAGE_REL_LLVM_FUSED_READ __imp_x$tls
// X64-NEXT: 0x7 IMAGE_REL_LLVM_FUSED_ACCESS __imp_x$tls
// X64-NEXT: 0xA IMAGE_REL_LLVM_FUSED_INDEX __imp_x$tls
// X64-NEXT: 0x19 IMAGE_REL_LLVM_FUSED_ACCESS __imp_x$tls
// X64-NEXT: 0x1C IMAGE_REL_LLVM_FUSED_INDEX __imp_x$tls
// X64-NEXT: 0x1F IMAGE_REL_LLVM_FUSED_INDEX __imp_x$tls
// A64:      0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_x$tls
// A64-NEXT: 0x4 IMAGE_REL_LLVM_FUSED_READ __imp_x$tls
// A64-NEXT: 0x8 IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL __imp_x$tls
// A64-NEXT: 0xC IMAGE_REL_LLVM_FUSED_INDEX __imp_x$tls
// A64-NEXT: 0x18 IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL __imp_x$tls
// A64-NEXT: 0x1C IMAGE_REL_LLVM_FUSED_INDEX __imp_x$tls
// A64-NEXT: 0x20 IMAGE_REL_LLVM_FUSED_INDEX __imp_x$tls

// FACTS:      Kind: SiteFacts
// FACTS-NEXT: SiteFact {
// FACTS-NEXT:   Section: .text
// X64-NEXT:     Relocation: 1
// A64-NEXT:     Relocation: 2
// FACTS-NEXT:   Fact: 9
// FACTS-NEXT:   Value: 0
// FACTS-NEXT: }
// FACTS-NEXT: SiteFact {
// FACTS-NEXT:   Section: .text
// X64-NEXT:     Relocation: 2
// A64-NEXT:     Relocation: 3
// FACTS-NEXT:   Fact: 8
// FACTS-NEXT:   Value: 0
// FACTS-NEXT: }
// FACTS:        Fact: 9
// FACTS:        Fact: 8
// FACTS-NEXT:   Value: 8
// FACTS:        Fact: 8
// FACTS-NEXT:   Value: 24
// FACTS-NOT:  SiteFact
// FACTS:      Flags: 0x8
.ifndef A64
  movq __imp_x$tls@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
  .site_fact 9, 0
  movq (%rax), %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 0
  movl (%rcx), %ecx
  movq %gs:88, %rdx
  movq (%rdx,%rcx,8), %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
  .site_fact 9, 0
  movl 8(%rax), %eax
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 8
  movl (%rcx,%rax), %edx
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 24
  leaq 4(%rax,%rcx), %rax
  retq
.else
  adrp x8, __imp_x$tls
  ldr x8, [x8, :fused_lo12:__imp_x$tls]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
  .site_fact 9, 0
  ldr x9, [x8]
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 0
  ldr w9, [x9]
  ldr x10, [x18, #88]
  ldr x9, [x10, x9, lsl #3]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
  .site_fact 9, 0
  ldr w8, [x8, #8]
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 8
  ldr w0, [x9, x8]
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 24
  add x0, x8, x9
  ret
.endif
.else

// An index states its word, and its instruction reads that word as the fact
// says: the address in an access, and an offset in the register that the
// fact selects, which a marked access of the pointer last wrote.
.ifndef A64
  movq __imp_x$tls@FUSED(%rip), %rax
  movq %gs:88, %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
  movl 8(%rax), %eax
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused index does not name a pointer at a use of the word of a record that its fact states
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  movl (%rcx,%rax), %edx
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused index does not name a pointer at a use of the word of a record that its fact states
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 8
  movl (%rax), %edx
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused index reads RCX, which no load of '__imp_x$tls' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 24
  movl (%rcx,%rax), %edx
.else
  adrp x8, __imp_x$tls
  ldr x8, [x8, :fused_lo12:__imp_x$tls]
  ldr x9, [x18, #88]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_x$tls
  ldr w8, [x8, #8]
// A64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused index does not name a pointer at a use of the word of a record that its fact states
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  ldr w0, [x9, x8]
// A64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused index does not name a pointer at a use of the word of a record that its fact states
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 8
  ldr w0, [x8]
// A64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused index reads X9, which no load of '__imp_x$tls' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
  .site_fact 8, 24
  ldr w0, [x9, x8]
.endif
.endif
.endif
