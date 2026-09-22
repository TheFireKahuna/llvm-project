// UCRT identity must suffice for the wrappers without the MinGW CRT macro.
// Two C translation units catch externally emitted SDK inline helpers.
// RUN: %clang_crt_main -std=c17 -O0 -UNDEBUG -Werror=implicit-function-declaration %s -DSECOND_UNIT -c -o %t.o
// RUN: %clang_crt_main -std=c17 -O0 -UNDEBUG -Werror=implicit-function-declaration %s %t.o -o %t.exe
// RUN: %run %t.exe
// RUN: %clangxx_crt_main -x c++ -std=c++17 -O0 -UNDEBUG -D_MSC_EXTENSIONS=1 %s -DSECOND_UNIT -c -o %t.cxx.o
// RUN: %clangxx_crt_main -x c++ -std=c++17 -O0 -UNDEBUG -D_MSC_EXTENSIONS=1 %s -x none %t.cxx.o -o %t.cxx.exe
// RUN: %run %t.cxx.exe
// REQUIRES: windows, crt

#undef __MSVCRT__

#ifdef __cplusplus
#  include <__config>
#  ifndef _LIBCPP_WIN32API
#    error Windows Itanium must select Win32 before including CRT headers.
#  endif
#endif

// malloc.h must work first, including establishing the SDK's UCRT marker.
#include <malloc.h>
#ifndef _UCRT
#  error The SDK must establish UCRT identity.
#endif
#include <assert.h>
#include <conio.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <memory.h>
#include <new.h>
#include <process.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(__cplusplus) && !defined(_MSC_EXTENSIONS)
#  error new.h must restore the caller's extension macro.
#endif

#ifdef __MSVCRT__
#  error The test must not depend on the MinGW CRT marker.
#endif

#ifdef SECOND_UNIT
int second_unit(void) {
  void *p = _malloca(4096);
  assert(p);
  memset(p, 42, 4096);
  _freea(p);
  return _get_heap_handle() != 0;
}
#else
int second_unit(void);
int main(void) {
  assert(second_unit());
  void *small = _malloca(32);
  assert(small);
  memset(small, 17, 32);
  _freea(small);
  void *aligned = NULL;
  assert(posix_memalign(&aligned, 16, 17) == 0);
  free(aligned);
  aligned = aligned_alloc(16, 17);
  assert(aligned);
  free(aligned);
  assert(_query_new_mode() == 0 || _query_new_mode() == 1);
  assert(getpid() > 0);
  assert(strcasecmp("UCRT", "ucrt") == 0);
  char path[4096];
  assert(getcwd(path, sizeof(path)) == path);
  assert(fileno(stdout) >= 0);
  assert(sizeof(off_t) == 8 && sizeof(ssize_t) == sizeof(void *));
  assert(O_RDONLY == _O_RDONLY && S_IFREG == _S_IFREG);
  // Compile declarations in the remaining alias families without performing
  // interactive console operations or modifying the filesystem.
  (void)&getch;
  (void)&j0;
  (void)&lfind;
  (void)&memccpy;
  (void)&execv;
  return 0;
}
#endif
