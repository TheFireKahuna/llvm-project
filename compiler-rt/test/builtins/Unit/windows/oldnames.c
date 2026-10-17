// RUN: %clang_wincrt %s -o %t.exe
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS
// RUN: %run %t.exe

// The POSIX names of Universal CRT functions link through oldnames.lib, as
// imports of the underscored functions.

#include <string.h>

int getpid(void);
char *strdup(const char *);
int stricmp(const char *, const char *);
int strcmpi(const char *, const char *);
int isascii(int);

int main(void) {
  char *Copy = strdup("Name");
  return getpid() <= 0 || strcmp(Copy, "Name") || stricmp(Copy, "NAME") ||
         strcmpi(Copy, "nAmE") || !isascii('a');
}

// IMPORTS:     Name: ucrtbase.dll
// IMPORTS-DAG: Symbol: _getpid
// IMPORTS-DAG: Symbol: _strdup
// IMPORTS-DAG: Symbol: _stricmp
// IMPORTS-DAG: Symbol: __isascii
