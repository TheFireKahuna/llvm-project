//
// malloc.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The memory allocation library.
//
/* Only include this if we're compiling for the windows platform. */
#ifndef _WIN32_ITANIUM
#if __has_include_next(<malloc.h>)
#include_next <malloc.h>
#endif
#elif defined(__LLVM_LIBC__)
/* Itanium + llvm-libc: no UCRT, provide malloc/free via <stdlib.h>. */
#ifndef _INC_MALLOC
#define _INC_MALLOC
#include <stdlib.h>
/* _alloca is a compiler builtin. */
#define _alloca __builtin_alloca
#define alloca __builtin_alloca

/* Heap walking stubs — _heapwalk/_HEAPINFO are UCRT debug APIs.
   With llvm-libc there is no CRT heap walker; report nothing. */
#define _HEAPEMPTY (-1)
#define _HEAPOK (-2)
#define _HEAPEND (-5)
#define _FREEENTRY 0
#define _USEDENTRY 1

typedef struct _heapinfo {
  int *_pentry;
  size_t _size;
  int _useflag;
} _HEAPINFO;

static __inline int _heapwalk(_HEAPINFO *info) {
  (void)info;
  return _HEAPEND; /* No entries — terminates immediately. */
}

#endif /* _INC_MALLOC */
#else
/* UCRT owns these declarations and the _malloca/_freea implementation. */
#include <corecrt.h>
#include <corecrt_malloc.h>
/* The SDK's C inline helpers have no exported out-of-line definitions. */
#if !defined(__cplusplus)
#pragma push_macro("__inline")
#undef __inline
#define __inline static __inline
#endif
#include_next <malloc.h>
#if !defined(__cplusplus)
#pragma pop_macro("__inline")
#endif
/* Let Clang's <mm_malloc.h> provide the free-compatible intrinsic pair. */
#undef _mm_malloc
#undef _mm_free
#endif // _WIN32_ITANIUM / __LLVM_LIBC__ / UCRT
