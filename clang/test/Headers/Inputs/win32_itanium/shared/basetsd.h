// A stand-in for the Windows SDK's basetsd.h, which defines its pointer
// conversion helpers __inline.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
__inline unsigned long PtrToUlong(const void *p) {
  return (unsigned long)(unsigned long long)p;
}
#ifdef __cplusplus
}
#endif
