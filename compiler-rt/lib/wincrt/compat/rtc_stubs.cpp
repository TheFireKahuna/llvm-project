//===-- rtc_stubs.cpp - Runtime Check stubs for /RTC builds ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stub implementations for MSVC Runtime Checks (/RTC1, /RTCs, /RTCu).
//
// These functions are called by code compiled with /RTC flags for debug builds.
// vcruntime provides the real implementations; we provide stubs that terminate
// with a clear error message since RTC is an MSVC-specific debug feature.
//
// Symbols provided:
//   _RTC_Initialize       - Initialize RTC subsystem
//   _RTC_Terminate        - Terminate RTC subsystem
//   _RTC_InitBase         - Initialize RTC with base address
//   _RTC_Shutdown         - Shutdown RTC
//   _RTC_CheckEsp         - Check stack pointer alignment (/RTCs)
//   _RTC_CheckStackVars   - Check for stack corruption (/RTCs)
//   _RTC_CheckStackVars2  - Check for stack corruption (v2)
//   _RTC_UninitUse        - Detect use of uninitialized variable (/RTCu)
//   _RTC_Failure          - Generic RTC failure handler
//   _RTC_SetErrorFunc     - Set custom error handler
//   _RTC_SetErrorFuncW    - Set custom error handler (wide)
//   _RTC_GetErrDesc       - Get error description
//   _RTC_NumErrors        - Get number of error types
//   _RTC_SetErrorType     - Set error type
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "../internal.h"

// Error codes for _RTC_Failure.
enum _RTC_ErrorNumber {
  _RTC_CHKSTK = 0,
  _RTC_CVRT_LOSS_INFO,
  _RTC_CORRUPT_STACK,
  _RTC_UNINIT_LOCAL_USE,
  _RTC_CORRUPTED_ALLOCA,
  _RTC_ILLEGAL
};

namespace {

// Early termination helper for RTC failures.
WINCRT_NORETURN void rtcFatalError(const char* msg) {
  OutputDebugStringA("WINCRT RTC: ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  wincrt::writeStderr("WINCRT RTC: ");
  wincrt::writeStderr(msg);
  wincrt::writeStderr("\n");

  // Use RaiseFailFastException for early termination.
  RaiseFailFastException(nullptr, nullptr, 0);
  __builtin_unreachable();
}

} // namespace

extern "C" {

// Error handler function pointer types.
using _RTC_error_fn = int(__cdecl *)(int errType, const char *file, int line,
                                     const char *module, const char *format,
                                     ...);
using _RTC_error_fnW = int(__cdecl *)(int errType, const wchar_t *file,
                                      int line, const wchar_t *module,
                                      const wchar_t *format, ...);

/// Initialize the RTC subsystem. Called before main() for /RTC builds.
/// Stub: no-op since RTC is not supported.
void __cdecl _RTC_Initialize(void) {}

/// Terminate the RTC subsystem. Called after main() for /RTC builds.
/// Stub: no-op since RTC is not supported.
void __cdecl _RTC_Terminate(void) {}

/// Initialize RTC with explicit base address.
/// Stub: no-op since RTC is not supported.
void __cdecl _RTC_InitBase(void) {}

/// Shutdown RTC subsystem.
/// Stub: no-op since RTC is not supported.
void __cdecl _RTC_Shutdown(void) {}

/// Check ESP register for corruption after function calls.
/// Called by /RTCs when calling functions with mismatched calling conventions.
///
/// If this is ever called, it means:
/// 1. Code was compiled with /RTCs
/// 2. A calling convention mismatch was detected
///
/// This should not happen in normal Itanium builds since we don't use /RTC.
WINCRT_NORETURN void __cdecl _RTC_CheckEsp(void) {
  rtcFatalError("RTC stack pointer check failed - /RTC is not supported");
}

/// Check for stack variable corruption at function exit.
/// Called by /RTCs to detect buffer overruns on stack variables.
///
/// Parameters:
///   frame - Pointer to stack frame
///   info  - Information about variables to check (compiler-generated)
WINCRT_NORETURN void __fastcall _RTC_CheckStackVars(void * /*frame*/,
                                                     void * /*info*/) {
  rtcFatalError("RTC stack corruption detected - /RTC is not supported");
}

/// Check for stack variable corruption (version 2).
WINCRT_NORETURN void __fastcall _RTC_CheckStackVars2(void * /*frame*/,
                                                      void * /*info*/,
                                                      void * /*context*/) {
  rtcFatalError("RTC stack corruption detected - /RTC is not supported");
}

/// Report use of an uninitialized local variable.
/// Called by /RTCu when reading from an uninitialized variable.
///
/// Parameters:
///   varname - Name of the uninitialized variable
WINCRT_NORETURN void __cdecl _RTC_UninitUse(const char *varname) {
  OutputDebugStringA("RTC: Use of uninitialized variable: ");
  if (varname)
    OutputDebugStringA(varname);
  OutputDebugStringA("\n");
  rtcFatalError("RTC uninitialized variable use - /RTC is not supported");
}

/// Generic RTC failure handler.
/// Called when any RTC check fails.
///
/// Parameters:
///   frame   - Pointer to stack frame
///   errType - Type of error (_RTC_ErrorNumber)
WINCRT_NORETURN void __fastcall _RTC_Failure(void * /*frame*/, int /*errType*/) {
  rtcFatalError("RTC failure - /RTC is not supported");
}

/// Set custom RTC error handler.
/// Stub: returns nullptr (no previous handler) and ignores the new handler.
_RTC_error_fn __cdecl _RTC_SetErrorFunc(_RTC_error_fn /*newFunc*/) {
  // RTC not supported; ignore handler and return nullptr.
  return nullptr;
}

/// Set custom RTC error handler (wide string version).
/// Stub: returns nullptr (no previous handler) and ignores the new handler.
_RTC_error_fnW __cdecl _RTC_SetErrorFuncW(_RTC_error_fnW /*newFunc*/) {
  // RTC not supported; ignore handler and return nullptr.
  return nullptr;
}

/// Get description of an RTC error type.
/// Returns nullptr since RTC is not supported.
const char *__cdecl _RTC_GetErrDesc(_RTC_ErrorNumber /*errnum*/) {
  return nullptr;
}

/// Get number of RTC error types.
/// Returns 0 since RTC is not supported.
int __cdecl _RTC_NumErrors(void) { return 0; }

/// Set the error type for reporting.
/// Stub: no-op since RTC is not supported.
int __cdecl _RTC_SetErrorType(int /*errnum*/, int /*type*/) { return 0; }

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
