//===-- gshandler_seh_windows.c - The /GS handler of a __try frame --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An object that MSVC compiles with /GS names __GSHandlerCheck_SEH as the
// language handler of a frame that holds a stack cookie and __try scopes. Its
// handler data is __C_specific_handler's scope table followed by the cookie
// word that __GSHandlerCheck reads, whose low two bits send the handler on to
// __C_specific_handler while dispatching and while unwinding. It is a member
// of its own, since only it needs __C_specific_handler.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

typedef struct {
  void *Unused[7];
  const uint32_t *HandlerData;
} DispatcherContext;

// The Windows convention, which NT-POSIX's C convention is not.
#define GS __attribute__((ms_abi, thread_pointer("none")))
__attribute__((ms_abi)) void
__GSHandlerCheckCommon(void *, const DispatcherContext *, const int32_t *);
__attribute__((ms_abi)) int __C_specific_handler(void *, void *, void *,
                                                 DispatcherContext *);

GS int __GSHandlerCheck_SEH(const uint32_t *Record, void *EstablisherFrame,
                            void *Context, DispatcherContext *Dispatch) {
  // The scope table's count, then 16 bytes a scope.
  const uint32_t *Scopes = Dispatch->HandlerData;
  const int32_t *Data = (const int32_t *)(Scopes + 1 + 4 * Scopes[0]);
  __GSHandlerCheckCommon(EstablisherFrame, Dispatch, Data);
  // EXCEPTION_UNWIND in ExceptionFlags.
  if (*Data & (Record[1] & 0x66 ? 2 : 1))
    return __C_specific_handler((void *)Record, EstablisherFrame, Context,
                                Dispatch);
  return 1; // ExceptionContinueSearch
}
