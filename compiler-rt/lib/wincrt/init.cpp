//===-- init.cpp - Image and executable start-up --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

struct _exception;

extern "C" {
// Exported by ntdll; no SDK header declares it.
NTSYSAPI void NTAPI RtlSetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER);

// A program's own math error handler, if it defines one.
int __cdecl _matherr(struct _exception *) __attribute__((weak));
}

namespace {

// Whether Address is a return address in one of the two functions that begin
// an Itanium exception's search for a handler, in the image that contains
// it. _Unwind_RaiseException may be inlined into the rethrow entry.
bool raisedByUnwinder(const void *Address) {
  void *Base;
  if (!RtlPcToFileHeader(const_cast<void *>(Address), &Base))
    return false;
  DWORD64 ImageBase;
  const RUNTIME_FUNCTION *Entry = RtlLookupFunctionEntry(
      reinterpret_cast<DWORD64>(Address), &ImageBase, nullptr);
  if (!Entry)
    return false;
  auto Function = reinterpret_cast<FARPROC>(ImageBase + Entry->BeginAddress);
  auto Module = static_cast<HMODULE>(Base);
  return Function == GetProcAddress(Module, "_Unwind_RaiseException") ||
         Function == GetProcAddress(Module, "_Unwind_Resume_or_Rethrow");
}

// The filter ntdll runs for an exception that no frame of its thread handled,
// at a thread's start, in the thread pool and at a fiber's start. An Itanium
// exception that gets here has no handler; resuming its raise makes
// _Unwind_RaiseException return, and __cxa_throw then calls std::terminate
// with nothing unwound, as the Itanium ABI requires. Every other exception
// goes where it went before: to the program's SetUnhandledExceptionFilter
// filter, then to Windows Error Reporting.
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *Exception) {
  const EXCEPTION_RECORD *Record = Exception->ExceptionRecord;
  if (Record->ExceptionCode == wincrt::GCCExceptionCode &&
      Record->NumberParameters == 1 && Record->ExceptionInformation[0] &&
      raisedByUnwinder(Record->ExceptionAddress))
    return EXCEPTION_CONTINUE_EXECUTION;
  return UnhandledExceptionFilter(Exception);
}

} // namespace

namespace wincrt {

// Start-up is single-threaded: a DLL's runs under the loader lock, and an
// executable's before main.
bool initializeImage() {
  if (_initterm_e(const_cast<_PIFV *>(__xi_a), const_cast<_PIFV *>(__xi_z)))
    return false;
  _initterm(const_cast<_PVFV *>(__xc_a), const_cast<_PVFV *>(__xc_z));
  return true;
}

void initializeExecutable() {
  // Only the executable installs the filter: a DLL could be unloaded while
  // the process-wide slot still pointed into it.
  RtlSetUnhandledExceptionFilter(unhandledExceptionFilter);
  if (_matherr)
    __setusermatherr(_matherr);
  if (!initializeImage())
    fatal("a C initializer failed");
}

void fatal(const char *Message) {
  static constexpr char Prefix[] = "wincrt: ";
  HANDLE Error = GetStdHandle(STD_ERROR_HANDLE);
  DWORD Written;
  if (Error && Error != INVALID_HANDLE_VALUE) {
    WriteFile(Error, Prefix, sizeof(Prefix) - 1, &Written, nullptr);
    WriteFile(Error, Message, static_cast<DWORD>(__builtin_strlen(Message)),
              &Written, nullptr);
    WriteFile(Error, "\n", 1, &Written, nullptr);
  }
  _exit(255);
}

} // namespace wincrt
