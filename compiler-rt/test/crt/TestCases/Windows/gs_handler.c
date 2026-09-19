// MSVC-built objects compiled with /GS register __GSHandlerCheck or
// __GSHandlerCheck_SEH as the language handler of a frame, with data that
// names the cookie slot. Clang never emits that shape, so the frames here are
// written in assembly in the three forms found in MSVC-built objects: a plain
// frame, a frame with a frame register at a scaled offset, and a __try frame
// whose scope table precedes the cookie word.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe plain 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe fp 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe seh 2>&1 | FileCheck %s --check-prefix=INNER
// RUN: %run %t.exe seh-nochain 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: not --crash %run %t.exe plain corrupt
// RUN: not --crash %run %t.exe fp corrupt
// RUN: not --crash %run %t.exe seh corrupt
// REQUIRES: windows, crt, x86_64

#include <stdio.h>
#include <string.h>
#include <windows.h>

// Each frame stores __security_cookie XORed with the register MSVC would use,
// runs the body, and checks the cookie on the way out. With Corrupt set it
// flips the slot before the body raises, which the handler must catch during
// dispatch, before any filter or outer handler runs.
void gs_frame_plain(void (*Body)(void), int Corrupt);
void gs_frame_fp(void (*Body)(void), int Corrupt);
int gs_frame_seh(void (*Body)(void), int Corrupt);
int gs_frame_seh_nochain(void (*Body)(void), int Corrupt);

LONG gs_filter(EXCEPTION_POINTERS *Pointers, void *Frame) {
  (void)Frame;
  fprintf(stderr, "filter %08lx\n", Pointers->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

__asm__(
    ".text\n"
    // Plain frame: cookie at [rsp+0x20], XORed with rsp after the prologue.
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

    // Frame register: rbp = rsp + 0x20 after the prologue, cookie at
    // [rbp+0x10] XORed with rbp. The establisher frame is rsp, so the data
    // names 0x30 and the handler adds the scaled frame offset for the XOR.
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

    // __try frame: a one-entry scope table, then the cookie word with bit 0
    // set so the handler chains to __C_specific_handler while dispatching.
    // The __except body returns 1. The nop keeps the call's return address
    // inside the range, as the compilers do.
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
    ".Lseh_try_begin:\n"
    "  callq *%rcx\n"
    "  nop\n"
    ".Lseh_try_end:\n"
    "  xorl %eax, %eax\n"
    "  jmp 2f\n"
    ".Lseh_except:\n"
    "  movl $1, %eax\n"
    "2:\n"
    "  movq 0x20(%rsp), %rcx\n"
    "  xorq %rsp, %rcx\n"
    "  movl %eax, 0x28(%rsp)\n"
    "  callq __security_check_cookie\n"
    "  movl 0x28(%rsp), %eax\n"
    "  addq $0x38, %rsp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck_SEH, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .long .Lseh_try_begin@IMGREL\n"
    "  .long .Lseh_try_end@IMGREL\n"
    "  .long gs_filter@IMGREL\n"
    "  .long .Lseh_except@IMGREL\n"
    "  .long 0x21\n"
    "  .text\n"
    ".seh_endproc\n"

    // The same frame with the chain bit clear: the filter is never consulted
    // and the exception reaches the caller.
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
    ".Lnochain_try_begin:\n"
    "  callq *%rcx\n"
    "  nop\n"
    ".Lnochain_try_end:\n"
    "  xorl %eax, %eax\n"
    "  jmp 2f\n"
    ".Lnochain_except:\n"
    "  movl $1, %eax\n"
    "2:\n"
    "  movq 0x20(%rsp), %rcx\n"
    "  xorq %rsp, %rcx\n"
    "  movl %eax, 0x28(%rsp)\n"
    "  callq __security_check_cookie\n"
    "  movl 0x28(%rsp), %eax\n"
    "  addq $0x38, %rsp\n"
    "  retq\n"
    "  .seh_handler __GSHandlerCheck_SEH, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .long .Lnochain_try_begin@IMGREL\n"
    "  .long .Lnochain_try_end@IMGREL\n"
    "  .long gs_filter@IMGREL\n"
    "  .long .Lnochain_except@IMGREL\n"
    "  .long 0x20\n"
    "  .text\n"
    ".seh_endproc\n");

static void raise_foreign(void) {
  RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static int outer_filter(unsigned long Code) {
  fprintf(stderr, "outer filter %08lx\n", Code);
  return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  int Corrupt = argc > 2 && strcmp(argv[2], "corrupt") == 0;
  int Result = -1;
  __try {
    if (strcmp(argv[1], "plain") == 0)
      gs_frame_plain(raise_foreign, Corrupt);
    else if (strcmp(argv[1], "fp") == 0)
      gs_frame_fp(raise_foreign, Corrupt);
    else if (strcmp(argv[1], "seh") == 0)
      Result = gs_frame_seh(raise_foreign, Corrupt);
    else if (strcmp(argv[1], "seh-nochain") == 0)
      Result = gs_frame_seh_nochain(raise_foreign, Corrupt);
    else
      return 2;
  } __except (outer_filter(GetExceptionCode())) {
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
