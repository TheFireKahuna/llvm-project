//===-- wincrt.h - Windows Itanium start-up internals -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// wincrt gives a Windows Itanium image what vcruntime gives an MSVC one: the
// entry points, the .CRT$X?? tables, the security cookie, the load
// configuration and the TLS directory. The C library is the Universal CRT,
// used through its public headers and import library only.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_WINCRT_H
#define COMPILER_RT_LIB_WINCRT_WINCRT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <corecrt_startup.h>
#include <stdlib.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "wincrt supports x86-64 and AArch64 only"
#endif

#define WINCRT_STRINGIFY(Name) #Name
#define WINCRT_ALTERNATENAME(From, To)                                         \
  __pragma(comment(linker, "/alternatename:" WINCRT_STRINGIFY(                 \
                               From) "=" WINCRT_STRINGIFY(To)))
#define WINCRT_INCLUDE(Name)                                                   \
  __pragma(comment(linker, "/include:" WINCRT_STRINGIFY(Name)))

extern "C" {
// The table bounds, defined by the builtins.
extern const _PIFV __xi_a[], __xi_z[];
extern const _PVFV __xc_a[], __xc_z[];

// The image's own header, defined by the linker.
extern IMAGE_DOS_HEADER __ImageBase;

void __cdecl __security_init_cookie(void);

// libc++abi: terminates with the given exception as the active one.
[[noreturn]] void __cxa_call_terminate(void *) noexcept;
}

// An image without libc++abi has no active exception to report.
WINCRT_ALTERNATENAME(__cxa_call_terminate, abort)

namespace wincrt {

// Runs the image's C initializers, then its C++ constructors. Returns false
// if a C initializer fails.
bool initializeImage();
// Executable start-up after the Universal CRT has its arguments and
// environment. Exits if a C initializer fails.
void initializeExecutable();
// Reports a start-up failure and exits without running any terminator.
[[noreturn, gnu::cold]] void fatal(const char *Message);

// The code an Itanium exception raises: "GCC" in ASCII.
constexpr DWORD GCCExceptionCode = 0x20474343;

// An Itanium exception that reaches a frame it must not pass terminates the
// program there, before anything is unwound, so that std::terminate sees it
// as the active exception with the thrower's frames intact. Every other
// exception goes on to the next handler.
inline int terminateFilter(EXCEPTION_POINTERS *Exception) {
  const EXCEPTION_RECORD *Record = Exception->ExceptionRecord;
  if (Record->ExceptionCode == GCCExceptionCode &&
      Record->NumberParameters == 1 && Record->ExceptionInformation[0])
    __cxa_call_terminate(
        reinterpret_cast<void *>(Record->ExceptionInformation[0]));
  return EXCEPTION_CONTINUE_SEARCH;
}

// The nCmdShow argument of WinMain: what the process's creator asked for.
inline int showWindowMode() {
  STARTUPINFOW Info;
  GetStartupInfoW(&Info);
  return Info.dwFlags & STARTF_USESHOWWINDOW ? Info.wShowWindow
                                             : SW_SHOWDEFAULT;
}

// The body of an executable's entry point. Each entry point is a separate
// translation unit that instantiates this with its own user entry, so that
// the linker extracts only the one the program needs and every call here is
// direct.
template <_crt_app_type Type, bool Wide, int (*Main)()>
[[noreturn]] void runExecutable() {
  __try {
    _set_app_type(Type);
    if constexpr (Wide) {
      if (_configure_wide_argv(_crt_argv_unexpanded_arguments) != 0 ||
          _initialize_wide_environment() != 0)
        fatal("not enough space for the arguments or the environment");
    } else {
      if (_configure_narrow_argv(_crt_argv_unexpanded_arguments) != 0 ||
          _initialize_narrow_environment() != 0)
        fatal("not enough space for the arguments or the environment");
    }
    initializeExecutable();
    exit(Main());
  } __except (_seh_filter_exe(GetExceptionCode(), GetExceptionInformation())) {
    // The Universal CRT turns a hardware exception into its signal; one it
    // chooses to handle here ends the program with the exception code.
    _exit(static_cast<int>(GetExceptionCode()));
  }
}

} // namespace wincrt

#endif // COMPILER_RT_LIB_WINCRT_WINCRT_H
