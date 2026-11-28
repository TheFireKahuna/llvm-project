// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe 2>&1 | FileCheck %s
// RUN: llvm-readobj --coff-imports --sections --coff-resources %t.exe | FileCheck %s \
// RUN:   --check-prefix=IMAGE --implicit-check-not=_initterm --implicit-check-not=.CRT
// RUN: %clang -DFAIL %s -o %t-fail.exe
// RUN: not %run %t-fail.exe 2>&1 | FileCheck %s --check-prefix=FAIL
// RUN: %python -c "import sys; open(sys.argv[2], 'wb').write(open(sys.argv[1], 'rb').read().replace(b'SegmentHeap', b'AnotherHeap'))" %t.exe %t-nt.exe
// RUN: not %run %t-nt.exe 2>&1 | FileCheck %s --check-prefix=NT

// The C initializers in .CRT$XI run in section order, then the constructors
// in .CRT$XC, then main, and at exit the pre-terminators in .CRT$XP and the
// terminators in .CRT$XT. The start-up code calls the entries itself, skipping
// the null entries the linker may pad the tables with, and does not import the
// Universal CRT's _initterm or _initterm_e. The tables are merged into .rdata.
// The builtins define each table's bounds, a null entry, the image's
// __dso_handle, the _fltused that objects compiled by Visual C++ name, and the
// __chkstk that a function with a large frame calls.
// [FAIL] A C initializer that fails ends start-up with an error, and nothing
// after it runs. [NT] The driver embeds the segment-heap manifest, and an
// executable whose manifest selects no segment heap fails before main.

// CHECK:      first initializer
// CHECK-NEXT: second initializer
// CHECK-NEXT: constructor
// CHECK-NEXT: main 0
// CHECK-NEXT: pre-terminator
// CHECK-NEXT: terminator

// IMAGE: Name: .rdata
// IMAGE: Name: ucrtbase.dll
// IMAGE: Type: MANIFEST (ID 24)

// FAIL:      first initializer
// FAIL-NEXT: second initializer
// FAIL-NEXT: wincrt: a C initializer failed
// FAIL-NOT:  {{.}}

// NT:     wincrt: the executable requires the segment heap, which its manifest selects
// NT-NOT: main

#include <stdint.h>
#include <stdio.h>

typedef int (*InitializerFn)(void);
typedef void (*TableFn)(void);

extern const InitializerFn __xi_a[], __xi_z[];
extern const TableFn __xc_a[], __xc_z[], __xp_a[], __xp_z[], __xt_a[], __xt_z[];
extern void *__dso_handle;
extern int _fltused;

static int first(void) { return fprintf(stderr, "first initializer\n"), 0; }
#ifdef FAIL
static int second(void) { return fprintf(stderr, "second initializer\n"), 1; }
#else
static int second(void) { return fprintf(stderr, "second initializer\n"), 0; }
#endif
static void constructor(void) { fprintf(stderr, "constructor\n"); }
static void preterminator(void) { fprintf(stderr, "pre-terminator\n"); }
static void terminator(void) { fprintf(stderr, "terminator\n"); }

#define ENTRY(Section, Type, Name, Function)                                   \
  __attribute__((used, section(Section))) static const Type Name = Function
ENTRY(".CRT$XIV", InitializerFn, Second, second);
ENTRY(".CRT$XIU", InitializerFn, First, first);
ENTRY(".CRT$XCU", TableFn, Null, 0);
ENTRY(".CRT$XCV", TableFn, Constructor, constructor);
ENTRY(".CRT$XPU", TableFn, Preterminator, preterminator);
ENTRY(".CRT$XTU", TableFn, Terminator, terminator);

__attribute__((noinline)) static int largeFrame(int Index) {
  volatile char Frame[3 * 4096];
  Frame[Index] = 1;
  return Frame[Index];
}

// The number of the first sentinel that is wrong, or 0.
static int sentinels(void) {
  // The linker orders the sections by name.
  const void *Bounds[] = {__xc_a, __xc_z, __xi_a, __xi_z, __xp_a, __xp_z, __xt_a, __xt_z};
  for (int I = 1; I < 8; ++I)
    if ((uintptr_t)Bounds[I - 1] >= (uintptr_t)Bounds[I])
      return I;
  if (__xi_a[0] || __xi_z[0] || __xc_a[0] || __xc_z[0] || __xp_a[0] || __xp_z[0] ||
      __xt_a[0] || __xt_z[0])
    return 10;
  if (__dso_handle != &__dso_handle)
    return 11;
  if (_fltused != 0x9875)
    return 12;
  return largeFrame(2 * 4096) != 1 ? 13 : 0;
}

int main(void) {
  fprintf(stderr, "main %d\n", sentinels());
  return 0;
}
