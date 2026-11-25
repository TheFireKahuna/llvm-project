// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception


// The runtime's own fault-probing accesses, of pages it may have returned:
// a u32's and a word's load, a copy of whole words and a word's
// compare-exchange. Each is
// a leaf, so a fault anywhere between the bounds resumes at the landing,
// which answers zero: the load reads zero, and the copy and the
// compare-exchange report that nothing was written.

// The object takes part in Control Flow Guard and lists its landing as an
// EH continuation target, which a context's resumption is checked against.
  .def @feat.00
  .scl 3
  .type 0
  .endef
  .globl @feat.00
.set @feat.00, 0x4800

  .text
  .globl __wincrt_probe_begin
  .globl __wincrt_probe_end
  .globl __wincrt_probe32
  .globl __wincrt_probe64
  .globl __wincrt_probe_copy
  .globl __wincrt_probe_cas
  .p2align 4
__wincrt_probe_begin:
#if defined(__x86_64__)
// uint32_t __wincrt_probe32(const void *)
__wincrt_probe32:
  movl (%rcx), %eax
  retq
// uint64_t __wincrt_probe64(const void *)
__wincrt_probe64:
  movq (%rcx), %rax
  retq
// bool __wincrt_probe_copy(void *To, const void *From, size_t Words)
__wincrt_probe_copy:
  testq %r8, %r8
  jz 2f
1:
  movq (%rdx), %rax
  movq %rax, (%rcx)
  addq $8, %rcx
  addq $8, %rdx
  decq %r8
  jnz 1b
2:
  movl $1, %eax
  retq
// uint64_t __wincrt_probe_cas(uint64_t *, uint64_t Expected, uint64_t New)
__wincrt_probe_cas:
  movq %rdx, %rax
  lock cmpxchgq %r8, (%rcx)
  retq
__wincrt_probe_end:
  .globl __wincrt_probe_landing
__wincrt_probe_landing:
  xorl %eax, %eax
  retq
#else
__wincrt_probe32:
  ldr w0, [x0]
  ret
__wincrt_probe64:
  ldr x0, [x0]
  ret
__wincrt_probe_copy:
  cbz x2, 2f
1:
  ldr x3, [x1], #8
  str x3, [x0], #8
  subs x2, x2, #1
  b.ne 1b
2:
  mov w0, #1
  ret
__wincrt_probe_cas:
  casal x1, x2, [x0]
  mov x0, x1
  ret
__wincrt_probe_end:
  .globl __wincrt_probe_landing
__wincrt_probe_landing:
  mov x0, #0
  ret
#endif

.section .gehcont$y,"dr"
  .symidx __wincrt_probe_landing
