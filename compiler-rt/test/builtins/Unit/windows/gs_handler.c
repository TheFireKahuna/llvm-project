// REQUIRES: x86_64-target-arch || aarch64-target-arch
// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe 2>&1 | FileCheck %s %if x86_64-target-arch %{--check-prefixes=CHECK,X64%}
// RUN: not --crash %run %t.exe plain return 2>&1 | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe plain raise 2>&1 | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: not --crash %run %t.exe seh raise 2>&1 | FileCheck %s --allow-empty --check-prefix=CORRUPT
// RUN: %if x86_64-target-arch %{ not --crash %run %t.exe fp raise 2>&1 | FileCheck %s --allow-empty --check-prefix=CORRUPT %}
// RUN: %if x86_64-target-arch %{ not --crash %run %t.exe aligned raise 2>&1 | FileCheck %s --allow-empty --check-prefix=CORRUPT %}
// RUN: %if x86_64-target-arch %{ not --crash %run %t.exe aligned-fp raise 2>&1 | FileCheck %s --allow-empty --check-prefix=CORRUPT %}

// A frame of an MSVC /GS object has its cookie checked on return, and by
// __GSHandlerCheck or __GSHandlerCheck_SEH when an exception is dispatched
// through it, before any filter runs; a corrupted cookie fails fast, with no
// output. Clang never emits such frames, so they are written here in assembly,
// in the shapes MSVC 19.51 emits. On x86-64: a plain frame, a frame register
// at a scaled offset, dynamically aligned locals with and without a frame
// register, and __try frames whose scope table precedes the cookie word; the
// cookie is XORed with the stack pointer after the prologue, or with the frame
// register when there is one, even when the aligned base locates it. On
// AArch64, __security_push_cookie allocates 16 bytes that the unwind codes
// record, the cookie is in the upper 8, and the handler data is its offset
// from the stack pointer at entry. Bit 0 of an SEH frame's cookie word sends
// the handler on to __C_specific_handler; without it the filter is never
// consulted and the exception reaches the caller.

// CHECK:    plain: returned outer e0000001
// CHECK:    seh: returned filter e0000001 inner 1
// CHECK:    seh-nochain: returned outer e0000001
// X64:      fp: returned outer e0000001
// X64:      aligned: returned outer e0000001
// X64:      aligned-fp: returned outer e0000001
// CORRUPT-NOT: {{filter|outer|inner|returned}}

#include <stdio.h>
#include <string.h>
#include <windows.h>

// Each frame stores the cookie, flips it if Corrupt is set, calls Body, and
// checks the cookie on the way out.
typedef int Frame(void (*Body)(void), int Corrupt);
Frame gs_frame_plain, gs_frame_seh, gs_frame_seh_nochain;

LONG gs_filter(EXCEPTION_POINTERS *Pointers, void *Establisher) {
  fprintf(stderr, " filter %08lx", Pointers->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

#define FUNCTION(Name)                                                         \
  ".globl " Name "\n.def " Name "; .scl 2; .type 32; .endef\n.seh_proc " Name  \
  "\n" Name ":\n"
#define HANDLER(Handler, Data)                                                 \
  "  .seh_handler " Handler "\n  .seh_handlerdata\n" Data "  .text\n.seh_endproc\n"
#define SEH_HANDLER                                                            \
  HANDLER("__GSHandlerCheck_SEH, @except",                                     \
          "  .long 1\n  .rva \\name\\()_begin, \\name\\()_end, gs_filter, "     \
          "\\name\\()_except\n  .long \\word\n")

#if defined(__x86_64__)
Frame gs_frame_fp, gs_frame_aligned, gs_frame_aligned_fp;

__asm__(
    ".text\n"
    ".macro gs_store xor, slot\n"
    "  movq __security_cookie(%rip), %rax\n  xorq \\xor, %rax\n  movq %rax, \\slot\n"
    "  testl %edx, %edx\n  jz 1f\n  notq \\slot\n1:\n"
    ".endm\n"
    ".macro gs_check xor, slot\n"
    "  movq \\slot, %rcx\n  xorq \\xor, %rcx\n  callq __security_check_cookie\n"
    ".endm\n"

    // Cookie at [rsp+0x20], XORed with rsp.
    FUNCTION("gs_frame_plain")
    "  subq $0x38, %rsp\n  .seh_stackalloc 0x38\n  .seh_endprologue\n"
    "  gs_store %rsp, 0x20(%rsp)\n  callq *%rcx\n  gs_check %rsp, 0x20(%rsp)\n"
    "  addq $0x38, %rsp\n  retq\n"
    HANDLER("__GSHandlerCheck, @except, @unwind", "  .long 0x20\n")

    // rbp = rsp + 0x20, cookie at [rbp+0x10] XORed with rbp. The data names
    // the offset from the establisher frame, rsp.
    FUNCTION("gs_frame_fp")
    "  pushq %rbp\n  .seh_pushreg %rbp\n  subq $0x40, %rsp\n  .seh_stackalloc 0x40\n"
    "  leaq 0x20(%rsp), %rbp\n  .seh_setframe %rbp, 0x20\n  .seh_endprologue\n"
    "  gs_store %rbp, 0x10(%rbp)\n  callq *%rcx\n  gs_check %rbp, 0x10(%rbp)\n"
    "  leaq 0x20(%rbp), %rsp\n  popq %rbp\n  retq\n"
    HANDLER("__GSHandlerCheck, @except, @unwind", "  .long 0x30\n")

    // Locals aligned to 64 bytes at rbp = (rsp + 0x60) & -64, with no frame
    // register: the cookie at [rbp+0x40] is XORed with rsp.
    FUNCTION("gs_frame_aligned")
    "  pushq %rbp\n  .seh_pushreg %rbp\n  subq $0xb0, %rsp\n  .seh_stackalloc 0xb0\n"
    "  .seh_endprologue\n  leaq 0x60(%rsp), %rbp\n  andq $-0x40, %rbp\n"
    "  gs_store %rsp, 0x40(%rbp)\n  callq *%rcx\n  gs_check %rsp, 0x40(%rbp)\n"
    "  addq $0xb0, %rsp\n  popq %rbp\n  retq\n"
    HANDLER("__GSHandlerCheck, @except, @unwind", "  .long 0x44, 0x60, 0x40\n")

    // The same aligned locals with r13 = rsp + 0x20 as the frame register:
    // the cookie is XORed with r13, not with the aligned base.
    FUNCTION("gs_frame_aligned_fp")
    "  pushq %r13\n  .seh_pushreg %r13\n  pushq %rbp\n  .seh_pushreg %rbp\n"
    "  subq $0xb8, %rsp\n  .seh_stackalloc 0xb8\n  leaq 0x20(%rsp), %r13\n"
    "  .seh_setframe %r13, 0x20\n  .seh_endprologue\n"
    "  leaq 0x60(%rsp), %rbp\n  andq $-0x40, %rbp\n"
    "  gs_store %r13, 0x40(%rbp)\n  callq *%rcx\n  gs_check %r13, 0x40(%rbp)\n"
    "  leaq 0x98(%r13), %rsp\n  popq %rbp\n  popq %r13\n  retq\n"
    HANDLER("__GSHandlerCheck, @except, @unwind", "  .long 0x44, 0x60, 0x40\n")

    // A one-entry scope table, then the cookie word. The nop keeps the call's
    // return address inside the scope, as compilers do. The __except body,
    // which returns 1, is an EH continuation target, as MSVC lists it, since
    // unwinding to an unlisted target fails fast where the table is enforced.
    ".macro gs_seh name, word\n"
    FUNCTION("\\name")
    "  subq $0x38, %rsp\n  .seh_stackalloc 0x38\n  .seh_endprologue\n"
    "  gs_store %rsp, 0x20(%rsp)\n"
    "\\name\\()_begin:\n  callq *%rcx\n  nop\n\\name\\()_end:\n"
    "  xorl %eax, %eax\n  jmp 2f\n\\name\\()_except:\n  movl $1, %eax\n"
    "2:\n  movl %eax, 0x28(%rsp)\n  gs_check %rsp, 0x20(%rsp)\n"
    "  movl 0x28(%rsp), %eax\n  addq $0x38, %rsp\n  retq\n"
    SEH_HANDLER
    ".section .gehcont$y, \"dr\"\n.symidx \\name\\()_except\n.text\n"
    ".endm\n"
    "gs_seh gs_frame_seh, 0x21\n"
    "gs_seh gs_frame_seh_nochain, 0x20\n");

#else

__asm__(
    ".text\n"
    ".macro gs_flip\n"
    "  cbz w1, 1f\n  ldr x16, [sp, #8]\n  mvn x16, x16\n  str x16, [sp, #8]\n1:\n"
    ".endm\n"
    ".macro gs_return\n"
    "  .seh_startepilogue\n  bl __security_pop_cookie\n  .seh_stackalloc 16\n"
    ".endm\n"

    // Entry sp - 16 holds lr, and the cookie is at entry sp - 24.
    FUNCTION("gs_frame_plain")
    "  str x30, [sp, #-16]!\n  .seh_save_reg_x x30, 16\n  bl __security_push_cookie\n"
    "  .seh_stackalloc 16\n  .seh_endprologue\n  gs_flip\n  blr x0\n  gs_return\n"
    "  ldr x30, [sp], #16\n  .seh_save_reg_x x30, 16\n  .seh_endepilogue\n  ret\n"
    HANDLER("__GSHandlerCheck, @except, @unwind", "  .long -24\n")

    // A frame record at entry sp - 16, then the cookie at entry sp - 24, and
    // a one-entry scope table before the cookie word. The __except body
    // returns 1.
    ".macro gs_seh name, word\n"
    FUNCTION("\\name")
    "  stp x29, x30, [sp, #-16]!\n  .seh_save_fplr_x 16\n  mov x29, sp\n  .seh_set_fp\n"
    "  bl __security_push_cookie\n  .seh_stackalloc 16\n  .seh_endprologue\n  gs_flip\n"
    "\\name\\()_begin:\n  blr x0\n  nop\n\\name\\()_end:\n"
    "  mov w0, #0\n  b 2f\n\\name\\()_except:\n  mov w0, #1\n2:\n  gs_return\n"
    "  ldp x29, x30, [sp], #16\n  .seh_save_fplr_x 16\n  .seh_endepilogue\n  ret\n"
    SEH_HANDLER
    ".endm\n"
    "gs_seh gs_frame_seh, -23\n"
    "gs_seh gs_frame_seh_nochain, -24\n");

#endif

static const struct {
  const char *Name;
  Frame *Run;
} Frames[] = {{"plain", gs_frame_plain},
              {"seh", gs_frame_seh},
              {"seh-nochain", gs_frame_seh_nochain},
#if defined(__x86_64__)
              {"fp", gs_frame_fp},
              {"aligned", gs_frame_aligned},
              {"aligned-fp", gs_frame_aligned_fp}
#endif
};

static void returnNormally(void) {}
static void raiseForeign(void) {
  RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static void run(Frame *F, int Raise, int Corrupt) {
  __try {
    int Result = F(Raise ? raiseForeign : returnNormally, Corrupt);
    if (Raise)
      fprintf(stderr, " inner %d", Result);
    else
      fprintf(stderr, " returned");
  } __except (fprintf(stderr, " outer %08lx", GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
  }
}

// With a frame's name and "return" or "raise", runs that case with the cookie
// corrupted; without, runs both cases of every frame.
int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  for (size_t I = 0; I != sizeof(Frames) / sizeof(Frames[0]); ++I) {
    if (argc == 3) {
      if (!strcmp(argv[1], Frames[I].Name))
        run(Frames[I].Run, !strcmp(argv[2], "raise"), 1);
      continue;
    }
    fprintf(stderr, "%s:", Frames[I].Name);
    run(Frames[I].Run, 0, 0);
    run(Frames[I].Run, 1, 0);
    fprintf(stderr, "\n");
  }
  return argc == 3;
}
