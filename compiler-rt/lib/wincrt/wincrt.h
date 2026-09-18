//===-- wincrt.h - Windows Itanium CRT startup internals -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// wincrt replaces vcruntime for Windows Itanium images. It supplies the image
// entry points, .CRT$X* section processing, the Itanium ABI termination hooks
// (__cxa_atexit, __cxa_thread_atexit_impl, __cxa_at_quick_exit and their
// finalizers), the /GS cookie, Control Flow Guard metadata and the PE load
// configuration. The C library is UCRT, consumed through its public headers
// and import library only; nothing here depends on vcruntime headers or
// libraries, and nothing re-declares what the Windows SDK already provides.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_WINCRT_H
#define COMPILER_RT_LIB_WINCRT_WINCRT_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <corecrt_startup.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "wincrt supports x86_64 and aarch64"
#endif

//===----------------------------------------------------------------------===//
// Linker directives
//===----------------------------------------------------------------------===//

#define WINCRT_SYM(Name) #Name

#define WINCRT_ALTERNATENAME(Weak, Strong)                                     \
  __pragma(comment(linker,                                                     \
                   "/alternatename:" WINCRT_SYM(Weak) "=" WINCRT_SYM(Strong)))
#define WINCRT_INCLUDE(Name)                                                   \
  __pragma(comment(linker, "/include:" WINCRT_SYM(Name)))

//===----------------------------------------------------------------------===//
// Lifetime owner selection
//===----------------------------------------------------------------------===//
//
// cxa_atexit.cpp and cxa_thread_atexit.cpp are compiled twice. Built into the
// shared C++ runtime (WINCRT_SHARED_CXX_RUNTIME) they define and export the
// process-wide registries under their ABI names. Built into clang_rt.wincrt
// they define the same code under __wincrt_local_* names, and the alternate
// names below select that copy only in images that do not import the shared
// runtime: C-only programs and the runtime DLLs themselves during bootstrap.

#ifdef WINCRT_SHARED_CXX_RUNTIME
#define WINCRT_LIFETIME_API __declspec(dllexport)
#define WINCRT_LIFETIME(Name) Name
#else
#define WINCRT_LIFETIME_API
#define WINCRT_LIFETIME(Name) __wincrt_local_##Name
#endif

extern "C" {
// Itanium C++ ABI 3.3.5 termination hooks and their wincrt extensions, as
// every image calls them. In the shared C++ runtime the exported definitions
// below are the only declarations, so the names are not declared twice with
// different storage classes.
#ifndef WINCRT_SHARED_CXX_RUNTIME
int __cdecl __cxa_atexit(void (*)(void *), void *, void *);
void __cdecl __cxa_finalize(void *);
int __cxa_at_quick_exit(void (*)(void), void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
void __cxa_thread_finalize(void *);
// Executable startup announces itself to the owner and supplies the hook
// that runs after the process-wide drain; DLL detach asks whether that
// image's registrations may be finalized now.
void __wincrt_register_executable(void (*)(void));
int __wincrt_detach_image(void *, int);
#endif
// libc++abi: terminate with the given unwind object as the active exception.
[[noreturn]] void __cxa_call_terminate(void *) noexcept;

// Definitions carry these names in the local copy.
WINCRT_LIFETIME_API int __cdecl WINCRT_LIFETIME(__cxa_atexit)(void (*)(void *),
                                                              void *, void *);
WINCRT_LIFETIME_API void __cdecl WINCRT_LIFETIME(__cxa_finalize)(void *);
WINCRT_LIFETIME_API int WINCRT_LIFETIME(__cxa_at_quick_exit)(void (*)(void),
                                                             void *);
WINCRT_LIFETIME_API int
    WINCRT_LIFETIME(__cxa_thread_atexit_impl)(void (*)(void *), void *, void *);
WINCRT_LIFETIME_API void WINCRT_LIFETIME(__cxa_thread_finalize)(void *);
WINCRT_LIFETIME_API void
    WINCRT_LIFETIME(__wincrt_register_executable)(void (*)(void));
WINCRT_LIFETIME_API int WINCRT_LIFETIME(__wincrt_detach_image)(void *, int);
}

#ifndef WINCRT_SHARED_CXX_RUNTIME
WINCRT_ALTERNATENAME(__cxa_atexit, __wincrt_local___cxa_atexit)
WINCRT_ALTERNATENAME(__cxa_finalize, __wincrt_local___cxa_finalize)
WINCRT_ALTERNATENAME(__cxa_at_quick_exit, __wincrt_local___cxa_at_quick_exit)
WINCRT_ALTERNATENAME(__cxa_thread_atexit_impl,
                     __wincrt_local___cxa_thread_atexit_impl)
WINCRT_ALTERNATENAME(__cxa_thread_finalize,
                     __wincrt_local___cxa_thread_finalize)
WINCRT_ALTERNATENAME(__wincrt_register_executable,
                     __wincrt_local___wincrt_register_executable)
WINCRT_ALTERNATENAME(__wincrt_detach_image,
                     __wincrt_local___wincrt_detach_image)
// A C-only image has no terminate handler; abort ignores the argument.
WINCRT_ALTERNATENAME(__cxa_call_terminate, abort)
#endif

//===----------------------------------------------------------------------===//
// Image metadata provided by builtins (crt_begin_windows.c, crt_end_windows.c,
// crt_pseudo_reloc_windows.cpp) and by the compiler
//===----------------------------------------------------------------------===//

extern "C" {
extern void *__dso_handle;
extern _PIFV __xi_a[], __xi_z[]; // .CRT$XIA..XIZ: C initializers
extern _PVFV __xc_a[], __xc_z[]; // .CRT$XCA..XCZ: C++ constructors
extern _PVFV __xp_a[], __xp_z[]; // .CRT$XPA..XPZ: pre-terminators
extern _PVFV __xt_a[], __xt_z[]; // .CRT$XTA..XTZ: terminators
void _pei386_runtime_relocator(void);
void __cdecl __security_init_cookie(void);
[[noreturn]] void __cdecl _amsg_exit(int);
}

//===----------------------------------------------------------------------===//
// Startup and termination helpers (init.cpp)
//===----------------------------------------------------------------------===//

namespace wincrt {

// UCRT _amsg_exit codes (_RT_* in the SDK sources).
enum class RuntimeError : int { SpaceArg = 8, SpaceEnv = 9, CrtNotInit = 30 };
[[noreturn]] void fatalError(RuntimeError);

// Runs the image's C initializers and C++ constructors once. Returns false
// when a C initializer fails.
bool initializeImage();
// Executable startup: registers UCRT exit processing, then initializes.
void executableInit();
// DLL detach: finalizes this image's registrations when the lifetime owner
// allows it, then runs the image's terminators.
void detachImage(bool Terminating);
// Configures UCRT, initializes, and runs the program's main function. Never
// returns; a caller must initialize the security cookie first.
[[noreturn]] void runExecutable(_crt_app_type Type,
                                int(__cdecl *ConfigureArgv)(_crt_argv_mode),
                                int(__cdecl *InitializeEnvironment)(void),
                                int (*Invoke)(void));

// STATUS_GCC_THROW: an Itanium exception with no handler reached a CRT frame.
// Hand the unwind object to libc++abi so std::terminate sees the active
// exception. Every other SEH exception continues to the next handler.
inline int terminateFilter(EXCEPTION_POINTERS *Exception) {
  const EXCEPTION_RECORD *Record = Exception->ExceptionRecord;
  if (Record->ExceptionCode == 0x20474343 && Record->NumberParameters == 1 &&
      Record->ExceptionInformation[0])
    __cxa_call_terminate(
        reinterpret_cast<void *>(Record->ExceptionInformation[0]));
  return EXCEPTION_CONTINUE_SEARCH;
}

#ifndef WINCRT_SHARED_CXX_RUNTIME
// Callback boundary for the local registries. The shared runtime calls its
// callbacks from noexcept functions instead, so libc++abi's terminate handler
// runs with the escaping exception active.
inline void invokeCallback(void (*Function)(void *), void *Object) {
  __try {
    Function(Object);
  } __except (terminateFilter(GetExceptionInformation())) {
    abort();
  }
}
#endif

//===----------------------------------------------------------------------===//
// Thread-local registry storage
//===----------------------------------------------------------------------===//

struct DtorEntry {
  void (*Dtor)(void *);
  void *Obj;
  void *Dso;
};

// The first block is embedded in its registry so the common case allocates
// nothing; later blocks come from the process heap.
constexpr size_t DtorBlockEntries = 32;

struct DtorBlock {
  DtorEntry Entries[DtorBlockEntries];
  DtorBlock *Next;
  size_t Count;

  bool hasSpace() const { return Count < DtorBlockEntries; }
  void push(void (*Dtor)(void *), void *Obj, void *Dso) {
    Entries[Count++] = {Dtor, Obj, Dso};
  }
};

inline void *crtAlloc(size_t Size) {
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

inline void crtFree(void *Memory) {
  if (Memory)
    HeapFree(GetProcessHeap(), 0, Memory);
}

} // namespace wincrt

#endif // COMPILER_RT_LIB_WINCRT_WINCRT_H
