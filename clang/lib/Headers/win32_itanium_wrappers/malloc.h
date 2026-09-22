#include <corecrt.h>
#include <corecrt_malloc.h>
#if !defined(__cplusplus)
#pragma push_macro("__inline")
#undef __inline
#define __inline static __inline
#endif
#include_next <malloc.h>
#if !defined(__cplusplus)
#pragma pop_macro("__inline")
#endif
#undef _mm_malloc
#undef _mm_free
