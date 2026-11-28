//===-- init.cpp - Image and executable start-up --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

// Every table of .CRT is read-only, like .rdata, so it needs no section of its
// own.
#pragma comment(linker, "/merge:.CRT=.rdata")

struct _exception;

extern "C" {
// A program's own math error handler, if it defines one.
int __cdecl _matherr(struct _exception *) __attribute__((weak));

// Exported by ntdll; no SDK header declares it.
NTSYSAPI void NTAPI RtlSetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER);

// libunwind's two functions that raise an exception's search, where it is
// linked into the image, which then exports neither. The table is hidden, so
// no other image's stands in for it.
extern const void *const __unw_seh_raise_functions[2]
    __attribute__((weak, visibility("hidden")));
}

// Call the entries of the table [First, Last), skipping the null entries that
// the linker may pad it with, as the Universal CRT's _initterm does; the _PIFV
// form stops at the first entry that returns nonzero, as _initterm_e does. The
// entries come from every object in the image, objects built without KCFI
// among them, so the calls check no KCFI type, and handing the Universal CRT
// the tables instead would open the types of the entries in every image. The
// bounds are distinct objects that the linker places around the entries, which
// the empty asm hides from the optimizer.
__attribute__((no_sanitize("kcfi"))) static void runTable(const _PVFV *First,
                                                          const _PVFV *Last) {
  __asm__("" : "+r"(First));
  for (; First != Last; ++First)
    if (*First)
      (*First)();
}

__attribute__((no_sanitize("kcfi"))) static int runTable(const _PIFV *First,
                                                         const _PIFV *Last) {
  __asm__("" : "+r"(First));
  for (; First != Last; ++First)
    if (*First)
      if (int Result = (*First)())
        return Result;
  return 0;
}

// The image's pre-terminators and terminators.
static void __cdecl runTerminators() {
  runTable(__xp_a, __xp_z);
  runTable(__xt_a, __xt_z);
}

// The filter ntdll runs for an exception that no frame of its thread took.
// Every exception but the unwinder's raise goes where it went without this
// filter: to the program's SetUnhandledExceptionFilter filter, then to Windows
// Error Reporting.
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *Exception) {
  return wincrt::raisedByUnwinder(Exception->ExceptionRecord)
             ? EXCEPTION_CONTINUE_EXECUTION
             : UnhandledExceptionFilter(Exception);
}

namespace wincrt {

bool raisedByUnwinder(const EXCEPTION_RECORD *Record) {
  if (Record->ExceptionCode != ItaniumThrow || Record->NumberParameters != 1)
    return false;
  DWORD64 Base;
  const RUNTIME_FUNCTION *Entry = RtlLookupFunctionEntry(
      reinterpret_cast<DWORD64>(Record->ExceptionAddress), &Base, nullptr);
  if (!Entry)
    return false;
  auto *Function = reinterpret_cast<const void *>(Base + Entry->BeginAddress);
  if (__unw_seh_raise_functions && (Function == __unw_seh_raise_functions[0] ||
                                    Function == __unw_seh_raise_functions[1]))
    return true;
  // A caller that passes its thread pointer runs X$tp, which is X's body only
  // where X never reads it.
  static const char *const Names[] = {
      "_Unwind_RaiseException", "_Unwind_RaiseException$tp",
      "_Unwind_Resume_or_Rethrow", "_Unwind_Resume_or_Rethrow$tp"};
  auto Module = reinterpret_cast<HMODULE>(Base);
  for (const char *Name : Names)
    if (Function == GetProcAddress(Module, Name))
      return true;
  return false;
}

// Start-up is single-threaded: a DLL's runs under the loader lock, and an
// executable's before main.
bool initializeImage() {
  if (runTable(__xi_a, __xi_z))
    return false;
  runTable(__xc_a, __xc_z);
  return true;
}

void initializeExecutable() {
  // Only the executable installs the filter: a DLL could be unloaded while
  // the process's slot still named its copy.
  RtlSetUnhandledExceptionFilter(unhandledExceptionFilter);
  // Before any constructor, which may call exit. The Universal CRT runs its
  // atexit table newest first, so the registries' token, posted second, runs
  // the registrations before the terminators.
  if (_crt_atexit(runTerminators) || !__wincrt_register_executable())
    fatal("not enough space for the exit list");
  if (_matherr)
    __setusermatherr(_matherr);
  // Only the executable's manifest selects the segment heap, so a process
  // started without the executable's activation context would run on the NT
  // heap unnoticed. The heap manager keeps its signature at offset 0x10 of
  // every heap.
  if (static_cast<const uint32_t *>(GetProcessHeap())[4] != 0xDDEEDDEE)
    fatal("the executable requires the segment heap, which its manifest "
          "selects");
  if (!initializeImage())
    fatal("a C initializer failed");
}

void finalizeImage(bool Terminating) {
  if (__wincrt_detach_image(&__dso_handle, Terminating))
    runTerminators();
}

void fatal(const char *Message) {
  HANDLE Error = GetStdHandle(STD_ERROR_HANDLE);
  DWORD Written;
  const char *Parts[] = {"wincrt: ", Message, "\n"};
  for (const char *Part : Parts)
    WriteFile(Error, Part, static_cast<DWORD>(__builtin_strlen(Part)), &Written,
              nullptr);
  _exit(255);
}

} // namespace wincrt
