// RUN: %clang %s -lm -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS
// RUN: %clang -DOWN %s -o %t-own.exe
// RUN: %run %t-own.exe | FileCheck %s --check-prefix=OWN

// The POSIX names of Universal CRT functions, which its headers declare, link
// through oldnames.lib, as imports of the underscored functions. The Universal
// CRT declares onexit, _HUGE and HUGE, but ucrtbase.dll exports none of them;
// wincrt defines them. [OWN] A program's own onexit and HUGE take precedence,
// even in an image that links the rest of wincrt's registration functions.
// The math functions come from the Universal CRT, the math library that POSIX
// build systems name is empty, and an executable's _matherr receives the math
// library's errors.

// CHECK:      log 1
// CHECK-NEXT: names 0, HUGE 1, _HUGE 1
// CHECK-NEXT: onexit
// CHECK-NEXT: atexit

// IMPORTS:     Name: ucrtbase.dll
// IMPORTS-DAG: Symbol: _getpid
// IMPORTS-DAG: Symbol: _strdup
// IMPORTS-DAG: Symbol: _stricmp
// IMPORTS-DAG: Symbol: __isascii

// OWN:      own onexit
// OWN-NEXT: names 0, HUGE 0, _HUGE 1
// OWN-NEXT: atexit
// OWN-NOT:  {{^}}onexit

#include <ctype.h>
#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The header's is a macro.
#undef isascii
int isascii(int);

#ifdef OWN
onexit_t onexit(onexit_t Function) {
  printf("own onexit\n");
  return Function;
}
double HUGE = 1.0;
#endif

int _matherr(struct _exception *Exception) {
  printf("%s %d\n", Exception->name, Exception->type);
  return 1;
}

static int fromOnexit(void) { return printf("onexit\n"), 0; }
static void fromAtexit(void) { printf("atexit\n"); }

int main(void) {
  volatile double Argument = -1.0;
  (void)log(Argument);
  char *Copy = strdup("Name");
  int Names = getpid() <= 0 || strcmp(Copy, "Name") || stricmp(Copy, "NAME") ||
              strcmpi(Copy, "nAmE") || !isascii('a');
  atexit(fromAtexit);
  if (onexit(fromOnexit) != fromOnexit)
    return 1;
  printf("names %d, HUGE %d, _HUGE %d\n", Names, HUGE == HUGE_VAL, _HUGE == HUGE_VAL);
  return 0;
}
