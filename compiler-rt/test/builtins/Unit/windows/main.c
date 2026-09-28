// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe one "two words" | FileCheck %s

// The Universal CRT's arguments and environment reach main.

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp) {
  if (argc != __argc || argv != __argv || envp != _environ || argv[argc])
    return 1;
  for (int I = 1; I < argc; ++I)
    printf("%s\n", argv[I]);
  return 0;
}

// CHECK:      one
// CHECK-NEXT: two words
