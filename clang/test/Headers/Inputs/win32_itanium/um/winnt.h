#ifndef DECLSPEC_GUARD_SUPPRESS
#if _MSC_FULL_VER >= 181040116
#define DECLSPEC_GUARD_SUPPRESS __declspec(guard(suppress))
#else
#define DECLSPEC_GUARD_SUPPRESS
#endif
#endif
