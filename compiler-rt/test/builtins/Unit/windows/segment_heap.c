// RUN: %clang_wincrt %s -o %t.exe
// RUN: llvm-readobj --coff-resources %t.exe | FileCheck %s
// RUN: %run %t.exe

// The driver embeds the segment-heap manifest in an executable, whose
// start-up then finds the segment heap it requires.

int main(void) { return 0; }

// CHECK: Type: MANIFEST (ID 24)
