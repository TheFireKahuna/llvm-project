// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe one "two words" | FileCheck %s

// The linker picks wmainCRTStartup, and the wide arguments and environment
// reach wmain.

#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t **argv, wchar_t **envp) {
  if (argc != __argc || argv != __wargv || envp != _wenviron || argv[argc])
    return 1;
  for (int I = 1; I < argc; ++I)
    printf("%ls\n", argv[I]);
  return 0;
}

// CHECK:      one
// CHECK-NEXT: two words
