// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe plain 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe fp 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe aligned 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe aligned-fp 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe seh 2>&1 | FileCheck %s --check-prefix=INNER
// RUN: %run %t.exe seh-nochain 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: not --crash %run %t.exe plain corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe fp corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe aligned corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe aligned-fp corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe seh corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// REQUIRES: x86_64-target-arch

// An exception dispatched through a frame of an MSVC /GS object has the
// frame's cookie checked by __GSHandlerCheck or __GSHandlerCheck_SEH before
// any filter runs. Clang never emits such frames, so they are written here in
// assembly, in the shapes MSVC 19.51 emits: a plain frame, a frame register at
// a scaled offset, dynamically aligned locals with and without a frame
// register, and __try frames whose scope table precedes the cookie word. The
// cookie is XORed with the stack pointer after the prologue, or with the frame
// register when there is one, even when the aligned base locates the cookie.
// A corrupted cookie fails fast before any filter prints.

#include <stdio.h>
#include <string.h>
#include <windows.h>

// Each frame stores the cookie, flips it if Corrupt is set, calls Body, and
// checks the cookie on the way out.
void gs_frame_plain(void (*Body)(void), int Corrupt);
void gs_frame_fp(void (*Body)(void), int Corrupt);
void gs_frame_aligned(void (*Body)(void), int Corrupt);
void gs_frame_aligned_fp(void (*Body)(void), int Corrupt);
int gs_frame_seh(void (*Body)(void), int Corrupt);
int gs_frame_seh_nochain(void (*Body)(void), int Corrupt);

LONG gs_filter(EXCEPTION_POINTERS *Pointers, void *Frame) {
  (void)Frame;
  fprintf(stderr, "filter %08lx\n", Pointers->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

__asm__(
    ".text\n"

    // Cookie at [rsp+0x20], XORed with rsp.
    ".globl gs_frame_plain\n"
    ".def gs_frame_plain; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_plain\n"
    "gs_frame_plain:\n"
    "  subq $0x38, %rsp\n"
    "  .seh_stackalloc 0x38\n"
    "  .seh_endprologue\n"
    "  movq __security_cookie(%rip), %rax\n"
    "  xorq %rsp, %rax\n"
    "  movq %rax, 0x20(%rsp)\n"
    "  testl %edx, %edx\n"
    "  jz 1f\n"
    "  notq 0x20(%rsp)\n"
    "1:\n"
    "  callq *%rcx\n"
    "  movq 0x20(%rsp), %rcx\n"
    "  xorq %rsp, %rcx\n"
    "  callq __security_check_cookie\n"
    "  addq $0x38, %rsp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck, @except, @unwind\n"
    "  .seh_handlerdata\n"
    "  .long 0x20\n"
    "  .text\n"
    ".seh_endproc\n"

    // rbp = rsp + 0x20, cookie at [rbp+0x10] XORed with rbp. The data names
    // the offset from the establisher frame, rsp.
    ".globl gs_frame_fp\n"
    ".def gs_frame_fp; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_fp\n"
    "gs_frame_fp:\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  subq $0x40, %rsp\n"
    "  .seh_stackalloc 0x40\n"
    "  leaq 0x20(%rsp), %rbp\n"
    "  .seh_setframe %rbp, 0x20\n"
    "  .seh_endprologue\n"
    "  movq __security_cookie(%rip), %rax\n"
    "  xorq %rbp, %rax\n"
    "  movq %rax, 0x10(%rbp)\n"
    "  testl %edx, %edx\n"
    "  jz 1f\n"
    "  notq 0x10(%rbp)\n"
    "1:\n"
    "  callq *%rcx\n"
    "  movq 0x10(%rbp), %rcx\n"
    "  xorq %rbp, %rcx\n"
    "  callq __security_check_cookie\n"
    "  leaq 0x20(%rbp), %rsp\n"
    "  popq %rbp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck, @except, @unwind\n"
    "  .seh_handlerdata\n"
    "  .long 0x30\n"
    "  .text\n"
    ".seh_endproc\n"

    // Locals aligned to 64 bytes at rbp = (rsp + 0x60) & -64, with no frame
    // register: the cookie at [rbp+0x40] is XORed with rsp.
    ".globl gs_frame_aligned\n"
    ".def gs_frame_aligned; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_aligned\n"
    "gs_frame_aligned:\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  subq $0xb0, %rsp\n"
    "  .seh_stackalloc 0xb0\n"
    "  .seh_endprologue\n"
    "  leaq 0x60(%rsp), %rbp\n"
    "  andq $-0x40, %rbp\n"
    "  movq __security_cookie(%rip), %rax\n"
    "  xorq %rsp, %rax\n"
    "  movq %rax, 0x40(%rbp)\n"
    "  testl %edx, %edx\n"
    "  jz 1f\n"
    "  notq 0x40(%rbp)\n"
    "1:\n"
    "  callq *%rcx\n"
    "  movq 0x40(%rbp), %rcx\n"
    "  xorq %rsp, %rcx\n"
    "  callq __security_check_cookie\n"
    "  addq $0xb0, %rsp\n"
    "  popq %rbp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck, @except, @unwind\n"
    "  .seh_handlerdata\n"
    "  .long 0x44, 0x60, 0x40\n"
    "  .text\n"
    ".seh_endproc\n"

    // The same aligned locals with r13 = rsp + 0x20 as the frame register:
    // the cookie is XORed with r13, not with the aligned base.
    ".globl gs_frame_aligned_fp\n"
    ".def gs_frame_aligned_fp; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_aligned_fp\n"
    "gs_frame_aligned_fp:\n"
    "  pushq %r13\n"
    "  .seh_pushreg %r13\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  subq $0xb8, %rsp\n"
    "  .seh_stackalloc 0xb8\n"
    "  leaq 0x20(%rsp), %r13\n"
    "  .seh_setframe %r13, 0x20\n"
    "  .seh_endprologue\n"
    "  leaq 0x60(%rsp), %rbp\n"
    "  andq $-0x40, %rbp\n"
    "  movq __security_cookie(%rip), %rax\n"
    "  xorq %r13, %rax\n"
    "  movq %rax, 0x40(%rbp)\n"
    "  testl %edx, %edx\n"
    "  jz 1f\n"
    "  notq 0x40(%rbp)\n"
    "1:\n"
    "  callq *%rcx\n"
    "  movq 0x40(%rbp), %rcx\n"
    "  xorq %r13, %rcx\n"
    "  callq __security_check_cookie\n"
    "  leaq 0x98(%r13), %rsp\n"
    "  popq %rbp\n"
    "  popq %r13\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck, @except, @unwind\n"
    "  .seh_handlerdata\n"
    "  .long 0x44, 0x60, 0x40\n"
    "  .text\n"
    ".seh_endproc\n"

    // A one-entry scope table, then the cookie word with bit 0 set, so the
    // handler goes on to __C_specific_handler while dispatching. The __except
    // body returns 1. The nop keeps the call's return address inside the
    // scope, as compilers do.
    ".globl gs_frame_seh\n"
    ".def gs_frame_seh; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_seh\n"
    "gs_frame_seh:\n"
    "  subq $0x38, %rsp\n"
    "  .seh_stackalloc 0x38\n"
    "  .seh_endprologue\n"
    "  movq __security_cookie(%rip), %rax\n"
    "  xorq %rsp, %rax\n"
    "  movq %rax, 0x20(%rsp)\n"
    "  testl %edx, %edx\n"
    "  jz 1f\n"
    "  notq 0x20(%rsp)\n"
    "1:\n"
    ".Lseh_begin:\n"
    "  callq *%rcx\n"
    "  nop\n"
    ".Lseh_end:\n"
    "  xorl %eax, %eax\n"
    "  jmp 2f\n"
    ".Lseh_except:\n"
    "  movl $1, %eax\n"
    "2:\n"
    "  movl %eax, 0x28(%rsp)\n"
    "  movq 0x20(%rsp), %rcx\n"
    "  xorq %rsp, %rcx\n"
    "  callq __security_check_cookie\n"
    "  movl 0x28(%rsp), %eax\n"
    "  addq $0x38, %rsp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck_SEH, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .rva .Lseh_begin, .Lseh_end, gs_filter, .Lseh_except\n"
    "  .long 0x21\n"
    "  .text\n"
    ".seh_endproc\n"

    // The same frame with bit 0 clear: the filter is never consulted, and
    // the exception reaches the caller.
    ".globl gs_frame_seh_nochain\n"
    ".def gs_frame_seh_nochain; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_seh_nochain\n"
    "gs_frame_seh_nochain:\n"
    "  subq $0x38, %rsp\n"
    "  .seh_stackalloc 0x38\n"
    "  .seh_endprologue\n"
    "  movq __security_cookie(%rip), %rax\n"
    "  xorq %rsp, %rax\n"
    "  movq %rax, 0x20(%rsp)\n"
    ".Lnochain_begin:\n"
    "  callq *%rcx\n"
    "  nop\n"
    ".Lnochain_end:\n"
    "  xorl %eax, %eax\n"
    "  jmp 2f\n"
    ".Lnochain_except:\n"
    "  movl $1, %eax\n"
    "2:\n"
    "  movl %eax, 0x28(%rsp)\n"
    "  movq 0x20(%rsp), %rcx\n"
    "  xorq %rsp, %rcx\n"
    "  callq __security_check_cookie\n"
    "  movl 0x28(%rsp), %eax\n"
    "  addq $0x38, %rsp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck_SEH, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .rva .Lnochain_begin, .Lnochain_end, gs_filter, .Lnochain_except\n"
    "  .long 0x20\n"
    "  .text\n"
    ".seh_endproc\n");

static void raiseForeign(void) {
  RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static int outerFilter(unsigned long Code) {
  fprintf(stderr, "outer filter %08lx\n", Code);
  return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (argc < 2)
    return 2;
  int Corrupt = argc > 2 && strcmp(argv[2], "corrupt") == 0;
  int Result = -1;
  __try {
    if (strcmp(argv[1], "plain") == 0)
      gs_frame_plain(raiseForeign, Corrupt);
    else if (strcmp(argv[1], "fp") == 0)
      gs_frame_fp(raiseForeign, Corrupt);
    else if (strcmp(argv[1], "aligned") == 0)
      gs_frame_aligned(raiseForeign, Corrupt);
    else if (strcmp(argv[1], "aligned-fp") == 0)
      gs_frame_aligned_fp(raiseForeign, Corrupt);
    else if (strcmp(argv[1], "seh") == 0)
      Result = gs_frame_seh(raiseForeign, Corrupt);
    else if (strcmp(argv[1], "seh-nochain") == 0)
      Result = gs_frame_seh_nochain(raiseForeign, Corrupt);
    else
      return 2;
  } __except (outerFilter(GetExceptionCode())) {
    fprintf(stderr, "outer handler\n");
    return 0;
  }
  fprintf(stderr, "inner result %d\n", Result);
  return Result == 1 ? 0 : 1;
}

// OUTER-NOT: {{^filter}}
// OUTER: outer filter e0000001
// OUTER-NEXT: outer handler

// INNER: {{^filter e0000001}}
// INNER-NEXT: inner result 1

// CORRUPT-NOT: filter
