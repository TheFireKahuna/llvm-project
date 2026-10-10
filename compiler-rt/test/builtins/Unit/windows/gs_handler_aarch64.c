// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe return
// RUN: %run %t.exe plain 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: %run %t.exe seh 2>&1 | FileCheck %s --check-prefix=INNER
// RUN: %run %t.exe seh-nochain 2>&1 | FileCheck %s --check-prefix=OUTER
// RUN: not --crash %run %t.exe return corrupt
// RUN: not --crash %run %t.exe plain corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe seh corrupt 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CORRUPT
// REQUIRES: aarch64-target-arch

// An AArch64 frame of an MSVC /GS object stores its cookie through
// __security_push_cookie, checks it through __security_pop_cookie, and has it
// checked by __GSHandlerCheck or __GSHandlerCheck_SEH when an exception is
// dispatched through it. Clang never emits such frames, so they are written
// here in assembly, in the shapes MSVC 19.51 emits: the push allocates 16
// bytes that the unwind codes record, the cookie is in the upper 8, and the
// handler data is its offset from the stack pointer at entry. A corrupted
// cookie fails fast at the pop, or before any filter prints.

#include <stdio.h>
#include <string.h>
#include <windows.h>

// Each frame pushes the cookie, flips it if Corrupt is set, calls Body, and
// pops the cookie on the way out.
void gs_frame_plain(void (*Body)(void), int Corrupt);
int gs_frame_seh(void (*Body)(void), int Corrupt);
int gs_frame_seh_nochain(void (*Body)(void), int Corrupt);

LONG gs_filter(EXCEPTION_POINTERS *Pointers, void *Frame) {
  (void)Frame;
  fprintf(stderr, "filter %08lx\n", Pointers->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

__asm__(
    ".text\n"

    // Entry sp - 16 holds lr, and the cookie is at entry sp - 24.
    ".globl gs_frame_plain\n"
    ".def gs_frame_plain; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_plain\n"
    "gs_frame_plain:\n"
    "  str x30, [sp, #-16]!\n"
    "  .seh_save_reg_x x30, 16\n"
    "  bl __security_push_cookie\n"
    "  .seh_stackalloc 16\n"
    "  .seh_endprologue\n"
    "  cbz w1, 1f\n"
    "  ldr x16, [sp, #8]\n"
    "  mvn x16, x16\n"
    "  str x16, [sp, #8]\n"
    "1:\n"
    "  blr x0\n"
    "  .seh_startepilogue\n"
    "  bl __security_pop_cookie\n"
    "  .seh_stackalloc 16\n"
    "  ldr x30, [sp], #16\n"
    "  .seh_save_reg_x x30, 16\n"
    "  .seh_endepilogue\n"
    "  ret\n"
    "  .seh_handler __GSHandlerCheck, @except, @unwind\n"
    "  .seh_handlerdata\n"
    "  .long -24\n"
    "  .text\n"
    ".seh_endproc\n"

    // A frame record at entry sp - 16, then the cookie at entry sp - 24, and
    // a one-entry scope table before the cookie word, whose bit 0 is set so
    // the handler goes on to __C_specific_handler while dispatching. The
    // __except body returns 1.
    ".globl gs_frame_seh\n"
    ".def gs_frame_seh; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_seh\n"
    "gs_frame_seh:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  .seh_save_fplr_x 16\n"
    "  mov x29, sp\n"
    "  .seh_set_fp\n"
    "  bl __security_push_cookie\n"
    "  .seh_stackalloc 16\n"
    "  .seh_endprologue\n"
    "  cbz w1, 1f\n"
    "  ldr x16, [sp, #8]\n"
    "  mvn x16, x16\n"
    "  str x16, [sp, #8]\n"
    "1:\n"
    ".Lseh_begin:\n"
    "  blr x0\n"
    "  nop\n"
    ".Lseh_end:\n"
    "  mov w0, #0\n"
    "  b 2f\n"
    ".Lseh_except:\n"
    "  mov w0, #1\n"
    "2:\n"
    "  .seh_startepilogue\n"
    "  bl __security_pop_cookie\n"
    "  .seh_stackalloc 16\n"
    "  ldp x29, x30, [sp], #16\n"
    "  .seh_save_fplr_x 16\n"
    "  .seh_endepilogue\n"
    "  ret\n"
    "  .seh_handler __GSHandlerCheck_SEH, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .rva .Lseh_begin, .Lseh_end, gs_filter, .Lseh_except\n"
    "  .long -23\n"
    "  .text\n"
    ".seh_endproc\n"

    // The same frame with bit 0 clear: the filter is never consulted, and
    // the exception reaches the caller.
    ".globl gs_frame_seh_nochain\n"
    ".def gs_frame_seh_nochain; .scl 2; .type 32; .endef\n"
    ".seh_proc gs_frame_seh_nochain\n"
    "gs_frame_seh_nochain:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  .seh_save_fplr_x 16\n"
    "  mov x29, sp\n"
    "  .seh_set_fp\n"
    "  bl __security_push_cookie\n"
    "  .seh_stackalloc 16\n"
    "  .seh_endprologue\n"
    ".Lnochain_begin:\n"
    "  blr x0\n"
    "  nop\n"
    ".Lnochain_end:\n"
    "  mov w0, #0\n"
    "  b 2f\n"
    ".Lnochain_except:\n"
    "  mov w0, #1\n"
    "2:\n"
    "  .seh_startepilogue\n"
    "  bl __security_pop_cookie\n"
    "  .seh_stackalloc 16\n"
    "  ldp x29, x30, [sp], #16\n"
    "  .seh_save_fplr_x 16\n"
    "  .seh_endepilogue\n"
    "  ret\n"
    "  .seh_handler __GSHandlerCheck_SEH, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .rva .Lnochain_begin, .Lnochain_end, gs_filter, .Lnochain_except\n"
    "  .long -24\n"
    "  .text\n"
    ".seh_endproc\n");

static void returnNormally(void) {}

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
  if (strcmp(argv[1], "return") == 0) {
    gs_frame_plain(returnNormally, Corrupt);
    return 0;
  }
  int Result = -1;
  __try {
    if (strcmp(argv[1], "plain") == 0)
      gs_frame_plain(raiseForeign, Corrupt);
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
