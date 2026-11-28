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
// compare-exchange. Each is a leaf, so a fault anywhere between the bounds
// resumes at the landing, which answers zero: the load reads zero, and the
// copy and the compare-exchange report that nothing was written. The copy
// moves a word at a time below 48 words and by rep movsq from there on
// x86-64, keeping rdi and rsi in r9 and r10, where the runtime's handler
// takes them back from at a fault; on AArch64 32 bytes at a time after an
// odd word and an odd pair.

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
  cmpq $48, %r8
  jae 2f
1:
  movq -8(%rdx,%r8,8), %rax
  movq %rax, -8(%rcx,%r8,8)
  decq %r8
  jnz 1b
  movl $1, %eax
  retq
2:
  .globl __wincrt_probe_movs
  movq %rdi, %r9
  movq %rsi, %r10
  movq %rcx, %rdi
  movq %rdx, %rsi
  movq %r8, %rcx
__wincrt_probe_movs:
  rep movsq
  movq %r9, %rdi
  movq %r10, %rsi
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
  tbz x2, #0, 1f
  ldr x3, [x1], #8
  str x3, [x0], #8
  subs x2, x2, #1
  b.eq 3f
1:
  tbz x2, #1, 2f
  ldp x3, x4, [x1], #16
  stp x3, x4, [x0], #16
  subs x2, x2, #2
  b.eq 3f
2:
  ldp q0, q1, [x1], #32
  stp q0, q1, [x0], #32
  subs x2, x2, #4
  b.ne 2b
3:
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
  .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  vmovdqu %ymm\r, 32+32*\r(%rsp)
  .endr
  .set .Lat, 544
  .irp r,rcx,rdx,r8,r9,r10,r11
  movq %\r, .Lat(%rsp)
  .set .Lat, .Lat+8
  .endr
  movl $\how, %edx
  callq __wincrt_tp_cold
  .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  vmovdqu 32+32*\r(%rsp), %ymm\r
  .endr
  .set .Lat, 544
  .irp r,rcx,rdx,r8,r9,r10,r11
  movq .Lat(%rsp), %\r
  .set .Lat, .Lat+8
  .endr
  .seh_startepilogue
  addq $600, %rsp
  .seh_endepilogue
  retq
  .seh_endproc
.endm
#else
// x1 to x17 at sp, q0 to q31 above them, by pairs.
.macro pair op, r, base, scale, a, b
  \op \r\a, \r\b, [sp, #\base+\scale*\a]
.endm
.macro pairs op, sop
  .irp p,"1,2","3,4","5,6","7,8","9,10","11,12","13,14","15,16"
  pair \op, x, -8, 8, \p
  .endr
  \sop x17, [sp, #128]
  .irp p,"0,1","2,3","4,5","6,7","8,9","10,11","12,13","14,15","16,17","18,19","20,21","22,23","24,25","26,27","28,29","30,31"
  pair \op, q, 144, 16, \p
  .endr
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
  pairs stp, str
  mov w1, #\how
  bl __wincrt_tp_cold
  pairs ldp, ldr
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

// The personality of a function whose only landing pads are its fault
// probes', which the vectored handler recovers: it lets every search and
// every unwind pass.
  .globl __tp_personality
  .def __tp_personality
  .scl 2
  .type 32
  .endef
  .p2align 4
__tp_personality:
#if defined(__x86_64__)
  movl $1, %eax
#else
  mov w0, #1
#endif
  ret

  .section .drectve,"yni"
  .ascii " -export:__tp_slow -export:__tp_try -export:__tp_probe"
  .ascii " -export:__tp_personality"

  .section .gehcont$y,"dr"
  .symidx __wincrt_probe_landing
