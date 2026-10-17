// RUN: %clang_wincrt %s -o %t.exe
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS --implicit-check-not="Symbol: _purecall"
// RUN: %run %t.exe | FileCheck %s

// The image defines its own _purecall, rather than importing ucrtbase.dll's,
// and it calls the handler set with _set_purecall_handler.

#include <stdio.h>
#include <stdlib.h>

int _purecall(void);

static void handler(void) {
  printf("handler\n");
  fflush(stdout);
  _exit(0);
}

int main(void) {
  _set_purecall_handler(handler);
  _purecall();
  return 1;
}

// IMPORTS: Symbol: _get_purecall_handler
// CHECK:   handler
