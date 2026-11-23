// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The derivation's out-of-line calls, __tp_slow, __tp_try and __tp_probe,
// each of which preserves every register but the one that answers, so that
// an entry saves nothing for it on its hot path. Each saves what the C
// convention lets a callee change, the vector registers whole, and calls the
// runtime's one body with its form.
//
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

#if defined(__x86_64__)
.macro cold name, how
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 4
  .seh_proc \name
\name:
  subq $600, %rsp
  .seh_stackalloc 600
  .seh_endprologue
  movq %rcx, 544(%rsp)
  movq %rdx, 552(%rsp)
  movq %r8, 560(%rsp)
  movq %r9, 568(%rsp)
  movq %r10, 576(%rsp)
  movq %r11, 584(%rsp)
  .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  vmovdqu %ymm\r, 32+32*\r(%rsp)
  .endr
  movl $\how, %edx
  callq __wincrt_tp_cold
  .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  vmovdqu 32+32*\r(%rsp), %ymm\r
  .endr
  movq 544(%rsp), %rcx
  movq 552(%rsp), %rdx
  movq 560(%rsp), %r8
  movq 568(%rsp), %r9
  movq 576(%rsp), %r10
  movq 584(%rsp), %r11
  .seh_startepilogue
  addq $600, %rsp
  .seh_endepilogue
  retq
  .seh_endproc
.endm
#else
.macro qpairs op
  .irp p,"0,1","2,3","4,5","6,7","8,9","10,11","12,13","14,15","16,17","18,19","20,21","22,23","24,25","26,27","28,29","30,31"
  qpair \op, \p
  .endr
.endm
.macro qpair op, a, b
  \op q\a, q\b, [sp, #144+16*\a]
.endm
.macro cold name, how
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 2
  .seh_proc \name
\name:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  sub sp, sp, #656
  .seh_stackalloc 656
  .seh_endprologue
  stp x1, x2, [sp, #0]
  stp x3, x4, [sp, #16]
  stp x5, x6, [sp, #32]
  stp x7, x8, [sp, #48]
  stp x9, x10, [sp, #64]
  stp x11, x12, [sp, #80]
  stp x13, x14, [sp, #96]
  stp x15, x16, [sp, #112]
  str x17, [sp, #128]
  qpairs stp
  mov w1, #\how
  bl __wincrt_tp_cold
  qpairs ldp
  ldp x1, x2, [sp, #0]
  ldp x3, x4, [sp, #16]
  ldp x5, x6, [sp, #32]
  ldp x7, x8, [sp, #48]
  ldp x9, x10, [sp, #64]
  ldp x11, x12, [sp, #80]
  ldp x13, x14, [sp, #96]
  ldp x15, x16, [sp, #112]
  ldr x17, [sp, #128]
  .seh_startepilogue
  add sp, sp, #656
  .seh_stackalloc 656
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
  .seh_endproc
.endm
#endif

  cold __tp_slow, 0
  cold __tp_try, 1
  cold __tp_probe, 2

  .section .drectve,"yni"
  .ascii " -export:__tp_slow -export:__tp_try -export:__tp_probe"

  .section .gehcont$y,"dr"
  .symidx __wincrt_probe_landing
