//===-- init.cpp - Image and executable start-up --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

struct _exception;

// A program's own math error handler, if it defines one.
extern "C" int __cdecl _matherr(struct _exception *) __attribute__((weak));

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

namespace wincrt {

// Start-up is single-threaded: a DLL's runs under the loader lock, and an
// executable's before main.
bool initializeImage() {
  if (runTable(__xi_a, __xi_z))
    return false;
  runTable(__xc_a, __xc_z);
  return true;
}

void initializeExecutable() {
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
