// The ARM EHABI and pointer-authenticating personalities read other state.
// UNSUPPORTED: target={{(arm|thumb|arm64e).*}}
// RUN: %clang_builtins %s %librt -o %t && %run %t

// The C personality takes a landing pad from the LSDA's @LPStart, where the
// table states one, rather than from the start of the function.

#include <stdint.h>
#include <unwind.h>

static uint8_t text[64], lsda[16];
static uintptr_t landing;
static struct _Unwind_Exception e;

// The frame: a call 0x18 bytes into the function's text.
_Unwind_Word _Unwind_GetIP(struct _Unwind_Context *c) {
  return (uintptr_t)text + 0x19;
}
_Unwind_Ptr _Unwind_GetRegionStart(struct _Unwind_Context *c) {
  return (uintptr_t)text;
}
void *_Unwind_GetLanguageSpecificData(struct _Unwind_Context *c) {
  return lsda;
}
void _Unwind_SetGR(struct _Unwind_Context *c, int r, _Unwind_Word v) {}
void _Unwind_SetIP(struct _Unwind_Context *c, _Unwind_Word v) { landing = v; }

#if defined(__SEH__) && !defined(__USING_SJLJ_EXCEPTIONS__)
#if defined(__NTPOSIX__)
#define SEH_CC __attribute__((ms_abi))
#else
#define SEH_CC
#endif
SEH_CC int __gcc_personality_seh0(void *, void *, void *, void *);
// The bridge from the system's dispatcher runs the frame's cleanup phase.
SEH_CC int _GCC_specific_handler(void *r, void *f, void *c, void *d,
                                 _Unwind_Personality_Fn p) {
  return p(1, _UA_CLEANUP_PHASE, 0, &e, 0);
}
#define RUN_PERSONALITY() __gcc_personality_seh0(0, 0, 0, 0)
#else
_Unwind_Reason_Code __gcc_personality_v0(int, _Unwind_Action, uint64_t,
                                         struct _Unwind_Exception *,
                                         struct _Unwind_Context *);
#define RUN_PERSONALITY() __gcc_personality_v0(1, _UA_CLEANUP_PHASE, 0, &e, 0)
#endif

int main(void) {
  // @LPStart, pcrel sdata4, 32 bytes into the text; no types; one call site
  // over [16, 32) of the function, with its pad 5 bytes past @LPStart.
  lsda[0] = 0x1b;
  int32_t lpStart = (int32_t)((uintptr_t)(text + 32) - (uintptr_t)(lsda + 1));
  __builtin_memcpy(lsda + 1, &lpStart, sizeof lpStart);
  const uint8_t rest[] = {0xff, 0x01, 4, 16, 16, 5, 0};
  __builtin_memcpy(lsda + 5, rest, sizeof rest);
  RUN_PERSONALITY();
  return landing != (uintptr_t)text + 32 + 5;
}
