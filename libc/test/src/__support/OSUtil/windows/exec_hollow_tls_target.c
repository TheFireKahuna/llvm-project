/*===-- exec_hollow_tls_target.c — TLS test target for self-hollow ---------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===----------------------------------------------------------------------===
 *
 * Test target with TLS variables and TLS callbacks. Used to verify that
 * self-hollow correctly handles IMAGE_DIRECTORY_ENTRY_TLS.
 *
 * Build (with CRT, for TLS support):
 *   "C:/Program Files/LLVM/bin/clang.exe" -O2 -Wall                        \
 *       libc/test/src/__support/OSUtil/windows/exec_hollow_tls_target.c     \
 *       -lkernel32 -lntdll                                                  \
 *       -o exec_hollow_tls_target.exe
 *
 *===----------------------------------------------------------------------===*/

#ifndef _WIN32
#error "This test is Windows-only"
#endif

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "ntdll.lib")

#include <stdio.h>

/* TLS variable — compiler emits IMAGE_TLS_DIRECTORY */
__declspec(thread) int g_tls_value = 42;
__declspec(thread) int g_tls_callback_ran = 0;

/* TLS callback — invoked by the loader for DLL_PROCESS_ATTACH, etc.
 * After a self-hollow, this should NOT run automatically (no loader reinit),
 * so we check whether it ran as a diagnostic. */
void __stdcall tls_callback(void *DllHandle, unsigned long Reason,
                            void *Reserved) {
  if (Reason == 1 /* DLL_PROCESS_ATTACH */) {
    g_tls_callback_ran = 1;
  }
}

/* Register the TLS callback via the CRT's TLS callback mechanism */
#pragma data_seg(".CRT$XLB")
void (__stdcall *p_tls_callback)(void *, unsigned long, void *) = tls_callback;
#pragma data_seg()

/* Ensure the linker includes the TLS directory */
#ifdef _MSC_VER
#pragma comment(linker, "/INCLUDE:_tls_used")
#else
/* For clang/GCC, the CRT should handle this automatically */
#endif

typedef long NTSTATUS;
typedef void *HANDLE;
typedef unsigned long DWORD;
__declspec(dllimport) NTSTATUS __stdcall NtTerminateProcess(
    HANDLE ProcessHandle, NTSTATUS ExitStatus);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);

int main(void) {
  printf("\n");
  printf("================================================================\n");
  printf("  exec_hollow_tls_target — TLS TEST TARGET\n");
  printf("================================================================\n");
  printf("\n");
  printf("  PID:                %lu\n", (unsigned long)GetCurrentProcessId());
  printf("  g_tls_value:        %d (expect 42)\n", g_tls_value);
  printf("  g_tls_callback_ran: %d (expect 1 for normal, 0 for hollow)\n",
         g_tls_callback_ran);
  printf("\n");

  /* Modify TLS value to prove it's writable */
  g_tls_value = 99;
  printf("  g_tls_value after write: %d (expect 99)\n", g_tls_value);
  printf("\n");

  if (g_tls_value == 99) {
    printf("  [+] TLS variable read/write: PASS\n");
  } else {
    printf("  [-] TLS variable read/write: FAIL\n");
  }

  if (g_tls_callback_ran) {
    printf("  [+] TLS callback fired (normal launch)\n");
  } else {
    printf("  [i] TLS callback did NOT fire (expected for self-hollow)\n");
  }

  printf("\n");
  printf("================================================================\n");
  printf("\n");

  fflush(stdout);

  /* Use NtTerminateProcess to avoid CRT exit crash after hollow.
   * For standalone (non-hollow) launch, return 0 would also work,
   * but NtTerminateProcess is safe in both cases. */
  NtTerminateProcess((HANDLE)(long long)-1, (NTSTATUS)0);
  __builtin_unreachable();
}

/* GetCurrentProcessId declared above main() */
