// Verify pseudo-relocation runtime symbols are present.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

// Linker-provided pseudo-reloc list bounds. When no pseudo-relocs exist,
// these resolve to the empty sentinel via /alternatename.
extern char __RUNTIME_PSEUDO_RELOC_LIST__[];
extern char __RUNTIME_PSEUDO_RELOC_LIST_END__[];

// Guard preventing double-execution of pseudo-relocator.
extern volatile long __pseudo_reloc_guard;

// Public entry point called by CRT init.
void _pei386_runtime_relocator(void);

int main(void) {
  // CHECK: Pseudo-reloc symbols test
  printf("Pseudo-reloc symbols test\n");

  // Symbols should exist and be resolvable.
  // CHECK: list start exists = 1
  printf("list start exists = %d\n", __RUNTIME_PSEUDO_RELOC_LIST__ != 0);

  // CHECK: list end exists = 1
  printf("list end exists = %d\n", __RUNTIME_PSEUDO_RELOC_LIST_END__ != 0);

  // With no auto-imported symbols, list should be empty.
  // CHECK: list is empty = 1
  printf("list is empty = %d\n",
         __RUNTIME_PSEUDO_RELOC_LIST__ == __RUNTIME_PSEUDO_RELOC_LIST_END__);

  // Guard should be set to 1 after CRT init ran the relocator.
  // CHECK: guard executed = 1
  printf("guard executed = %d\n", __pseudo_reloc_guard == 1);

  // Calling again should be a no-op due to guard.
  _pei386_runtime_relocator();
  // CHECK: guard still 1 = 1
  printf("guard still 1 = %d\n", __pseudo_reloc_guard == 1);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
