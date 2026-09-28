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
#include <stdint.h>
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

// The termination registries are compiled twice. Built into the shared C++
// runtime, they are the process-wide registries and export the Itanium ABI
// names. Built into wincrt, they have local names, and the alternate names
// below select them only in an image that does not import the shared ones.
#ifdef WINCRT_SHARED_CXX_RUNTIME
#define WINCRT_LIFETIME_API __attribute__((visibility("default")))
#define WINCRT_LIFETIME(Name) Name
#else
#define WINCRT_LIFETIME_API
#define WINCRT_LIFETIME(Name) __wincrt_local_##Name
#endif

extern "C" {
// Defined by the builtins: the table bounds and the image's DSO handle.
extern const _PIFV __xi_a[], __xi_z[];
extern const _PVFV __xc_a[], __xc_z[];
extern const _PVFV __xp_a[], __xp_z[];
extern const _PVFV __xt_a[], __xt_z[];
extern void *__dso_handle;

// The image's own header, defined by the linker.
extern IMAGE_DOS_HEADER __ImageBase;

extern uintptr_t __security_cookie;
void __cdecl __security_init_cookie(void);

// libc++abi: terminates with the given exception as the active one.
[[noreturn]] void __cxa_call_terminate(void *) noexcept;

#ifndef WINCRT_SHARED_CXX_RUNTIME
int __cdecl __cxa_atexit(void (*)(void *), void *, void *);
int __cxa_at_quick_exit(void (*)(void), void *);
// The executable's start-up makes the registry live for the whole process
// and hands it the executable's terminators, which run after its drain.
void __wincrt_register_executable(void (*)(void));
// A DLL's detach runs its registrations, unless the process is terminating
// without having run them. Returns whether it ran them.
int __wincrt_detach_image(void *, int);
#endif

WINCRT_LIFETIME_API int __cdecl WINCRT_LIFETIME(__cxa_atexit)(void (*)(void *),
                                                              void *, void *);
WINCRT_LIFETIME_API void __cdecl WINCRT_LIFETIME(__cxa_finalize)(void *);
WINCRT_LIFETIME_API int WINCRT_LIFETIME(__cxa_at_quick_exit)(void (*)(void),
                                                             void *);
WINCRT_LIFETIME_API void
    WINCRT_LIFETIME(__wincrt_register_executable)(void (*)(void));
WINCRT_LIFETIME_API int WINCRT_LIFETIME(__wincrt_detach_image)(void *, int);
}

#ifndef WINCRT_SHARED_CXX_RUNTIME
WINCRT_ALTERNATENAME(__cxa_atexit, __wincrt_local___cxa_atexit)
WINCRT_ALTERNATENAME(__cxa_finalize, __wincrt_local___cxa_finalize)
WINCRT_ALTERNATENAME(__cxa_at_quick_exit, __wincrt_local___cxa_at_quick_exit)
WINCRT_ALTERNATENAME(__wincrt_register_executable,
                     __wincrt_local___wincrt_register_executable)
WINCRT_ALTERNATENAME(__wincrt_detach_image,
                     __wincrt_local___wincrt_detach_image)
#endif

// An image without libc++abi has no active exception to report.
WINCRT_ALTERNATENAME(__cxa_call_terminate, abort)

namespace wincrt {

// Runs the image's C initializers, then its C++ constructors. Returns false
// if a C initializer fails.
bool initializeImage();
// Executable start-up after the Universal CRT has its arguments and
// environment. Exits if a C initializer fails.
void initializeExecutable();
// DLL detach, after the user's DllMain: runs the image's registrations and
// its terminators.
void finalizeImage(bool Terminating);
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

using Destructor = void (*)(void *);

// A registered destructor is stored encoded, as the Universal CRT stores its
// own function tables, so that a write to the registry's memory cannot make
// the program call an address of the writer's choosing at exit.
inline uintptr_t encodePointer(Destructor Function) {
  uintptr_t Cookie = __security_cookie;
  return __builtin_rotateright64(reinterpret_cast<uintptr_t>(Function) ^ Cookie,
                                 Cookie & 63);
}

inline Destructor decodePointer(uintptr_t Value) {
  uintptr_t Cookie = __security_cookie;
  return reinterpret_cast<Destructor>(
      __builtin_rotateleft64(Value, Cookie & 63) ^ Cookie);
}

// Under kcfi, the destructors that reach the registries carry the type
// void(void *) salted "__cxa_dtor", and a call to one must use the same
// type. The attribute is accepted only in C, but clang applies it in C++
// too.
#if __has_feature(kcfi)
#define WINCRT_DTOR_SALT __attribute__((cfi_salt("__cxa_dtor")))
#else
#define WINCRT_DTOR_SALT
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
typedef void (*SaltedDestructor)(void *) WINCRT_DTOR_SALT;
#pragma clang diagnostic pop

// A registered destructor that exits by an exception terminates the program
// ([basic.start.term], [support.start.term]).
inline void invokeCallback(Destructor Function, void *Object) {
  __try {
    reinterpret_cast<SaltedDestructor>(Function)(Object);
  } __except (terminateFilter(GetExceptionInformation())) {
    // The filter never selects this handler.
    __builtin_unreachable();
  }
}

inline void *crtAlloc(size_t Size) {
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

inline void crtFree(void *Memory) { HeapFree(GetProcessHeap(), 0, Memory); }

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
