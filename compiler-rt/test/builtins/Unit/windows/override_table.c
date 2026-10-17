// An executable that replaces an overridable default checks the override
// table in its header page against the anchor that its link wrote, in its
// first TLS callback, before any of its code runs, and fails fast when they
// differ. The link names __ovr_check, which brings the check and the TLS
// directory; an executable that replaces nothing has neither. A helper writes
// the table that the program's link would, from the anchor that the program
// defines here, and then tampers with it.

// RUN: %python %S/Inputs/override_table.py stub %t.stub
// RUN: %clang_wincrt %s -Wl,-stub:%t.stub -Wl,-include:__ovr_check -o %t.exe
// RUN: %python %S/Inputs/override_table.py table %t.exe
// RUN: %python %S/Inputs/exit_status.py %t.exe | FileCheck %s
// CHECK: callback
// CHECK: main
// CHECK: exit code 0

// RUN: %clang_wincrt %s -DNO_ANCHOR -o %t-plain.exe
// RUN: %python %S/Inputs/exit_status.py %t-plain.exe | FileCheck --check-prefix=PLAIN %s
// RUN: llvm-readobj --file-headers %t-plain.exe | FileCheck --check-prefix=NOTLS %s
// PLAIN: main
// PLAIN: exit code 0
// NOTLS: TLSTableRVA: 0x0

// RUN: cp %t.exe %t-erased.exe
// RUN: %python %S/Inputs/override_table.py table %t-erased.exe erased
// RUN: %python %S/Inputs/exit_status.py %t-erased.exe | FileCheck --check-prefix=FAIL %s
// RUN: cp %t.exe %t-version.exe
// RUN: %python %S/Inputs/override_table.py table %t-version.exe version
// RUN: %python %S/Inputs/exit_status.py %t-version.exe | FileCheck --check-prefix=FAIL %s
// RUN: cp %t.exe %t-count.exe
// RUN: %python %S/Inputs/override_table.py table %t-count.exe count
// RUN: %python %S/Inputs/exit_status.py %t-count.exe | FileCheck --check-prefix=FAIL %s
// RUN: cp %t.exe %t-dropped.exe
// RUN: %python %S/Inputs/override_table.py table %t-dropped.exe dropped
// RUN: %python %S/Inputs/exit_status.py %t-dropped.exe | FileCheck --check-prefix=FAIL %s
// RUN: cp %t.exe %t-moved.exe
// RUN: %python %S/Inputs/override_table.py table %t-moved.exe moved
// RUN: %python %S/Inputs/exit_status.py %t-moved.exe | FileCheck --check-prefix=FAIL %s
// RUN: cp %t.exe %t-stray.exe
// RUN: %python %S/Inputs/override_table.py table %t-stray.exe stray
// RUN: %python %S/Inputs/exit_status.py %t-stray.exe | FileCheck --check-prefix=FAIL %s
// RUN: cp %t.exe %t-stub.exe
// RUN: %python %S/Inputs/override_table.py table %t-stub.exe stub
// RUN: %python %S/Inputs/exit_status.py %t-stub.exe | FileCheck --check-prefix=FAIL %s
// FAIL-NOT: callback
// FAIL-NOT: main
// FAIL: exit code 3221226505

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *replacement(size_t size) { return malloc(size); }

#ifndef NO_ANCHOR
// The program's own TLS callback, which runs after the check.
static void __stdcall programCallback(void *Module, unsigned long Reason,
                                      void *Reserved) {
  if (Reason == 1) {
    puts("callback");
    fflush(stdout);
  }
}
__attribute__((section(".CRT$XLB"), used)) void(
    __stdcall *const program_callback)(void *, unsigned long,
                                       void *) = programCallback;

// Slot 0 names a replacement, and slot 1 forwards to it.
struct anchor {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t mask;
  uint32_t reserved;
  const void *entry[24];
};
__attribute__((section(".ovra"), used)) const struct anchor __ovr_anchor = {
    .magic = 0x7B3AC2D5,
    .version = 1,
    .count = 24,
    .mask = 1,
    .entry = {(const void *)&replacement, (const void *)&replacement},
};
#endif

int main(void) {
  puts("main");
  return replacement(1) ? 0 : 1;
}
