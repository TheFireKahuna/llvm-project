// A stand-in for the Windows SDK's ntdef.h, which chooses its declaration
// macros by _MSC_VER unless they are defined already.
#pragma once
#ifndef DECLSPEC_ALIGN
#if _MSC_VER >= 1300
#define DECLSPEC_ALIGN(x) __declspec(align(x))
#else
#define DECLSPEC_ALIGN(x)
#endif
#endif
#ifndef DECLSPEC_NORETURN
#if _MSC_VER >= 1200
#define DECLSPEC_NORETURN __declspec(noreturn)
#else
#define DECLSPEC_NORETURN
#endif
#endif
typedef struct DECLSPEC_ALIGN(16) _NTDEF_ALIGNED {
  long long Low;
  long long High;
} NTDEF_ALIGNED;
DECLSPEC_NORETURN void NtdefExit(void);
