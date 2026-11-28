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
// configuration, the TLS directory and the termination registries. The C
// library is the Universal CRT, used through its public headers and import
// library only.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_WINCRT_H
#define COMPILER_RT_LIB_WINCRT_WINCRT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <appmodel.h>
#include <corecrt_startup.h>
#include <stdint.h>
#include <stdlib.h>
#include <winternl.h>

#include "ucrt_names.h"

#define WINCRT_ALTERNATENAME(From, To)                                         \
  __pragma(comment(linker, "/alternatename:" #From "=" #To))

// The exec and spawn functions, with the name prefix Prefix.
#define WINCRT_SPAWN_FORMS(X, Prefix)                                          \
  X(Prefix##execl)                                                             \
  X(Prefix##execle)                                                            \
  X(Prefix##execlp) X(Prefix##execlpe) X(Prefix##execv) X(Prefix##execve)      \
      X(Prefix##execvp) X(Prefix##execvpe) X(Prefix##spawnl)                   \
          X(Prefix##spawnle) X(Prefix##spawnlp) X(Prefix##spawnlpe)            \
              X(Prefix##spawnv) X(Prefix##spawnve) X(Prefix##spawnvp)          \
                  X(Prefix##spawnvpe)

// Every entry object wraps the Universal CRT functions that wincrt defines
// again, the exec and spawn functions wide, narrow and under the narrow ones'
// traditional names, so that each reference to them in the image, the
// Universal CRT's import included, reaches wincrt's definitions whatever
// libraries the link names. It wraps the functions that other import
// libraries offer too (ucrt_names.h), so that the image binds the C library's.
#define WINCRT_WRAP_NAME(Name, ...) " /wrap:" #Name
#define WINCRT_WRAP_UCRT                                                       \
  __pragma(comment(linker,                                                     \
                   "/wrap:exit /wrap:_exit /wrap:_Exit /wrap:quick_exit "      \
                   "/wrap:_beginthread /wrap:_beginthreadex "                  \
                   "/wrap:_endthread /wrap:_endthreadex /wrap:rand_s "         \
                   "/wrap:raise /wrap:abort /wrap:_assert "                    \
                   "/wrap:_wassert" WINCRT_SPAWN_FORMS(WINCRT_WRAP_NAME, _)    \
                       WINCRT_SPAWN_FORMS(WINCRT_WRAP_NAME, _w)                \
                           WINCRT_SPAWN_FORMS(WINCRT_WRAP_NAME, )              \
                               WINCRT_NTDLL_FORMATTED_IO(WINCRT_WRAP_NAME)     \
                                   WINCRT_UCRT_SHARED(WINCRT_WRAP_NAME)))

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
}

// The parts of wincrt that serve the whole process: the termination
// registries, exit, exec, raise, abort, assert, the thread start and rand_s.
// Every image imports them from clang_rt.wincrt_dynamic.dll, so that
// registration is process-wide and finalization per image; a program linked
// with -static keeps them in the executable, from clang_rt.wincrt_static.lib.
#ifdef COMPILER_RT_SHARED_LIB
#define WINCRT_API __declspec(dllexport)
#else
#define WINCRT_API
#endif

extern "C" {
WINCRT_API int __cdecl __cxa_atexit(void (*)(void *), void *, void *);
WINCRT_API int __cdecl __llvm_kcfi_cxa_atexit(void (*)(void *), void *, void *);
WINCRT_API int __cdecl __wincrt_atexit(void (*)(void *), void *, void *);
WINCRT_API void __cdecl __cxa_finalize(void *);
WINCRT_API int __cxa_at_quick_exit(void (*)(void), void *);
WINCRT_API int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
WINCRT_API int __llvm_kcfi_cxa_thread_atexit_impl(void (*)(void *), void *,
                                                  void *);
// Runs the calling thread's thread-local destructors: those of one image, or
// all of them, completing the thread, if the argument is null.
WINCRT_API void __cxa_thread_finalize(void *);
// The executable's start-up makes the registries live for the whole process.
// Returns whether the Universal CRT took their tokens.
WINCRT_API int __wincrt_register_executable(void);
// A DLL's detach runs its registrations, unless the process is terminating
// without having run them. Returns whether it ran them.
WINCRT_API int __wincrt_detach_image(void *, int);
}

namespace wincrt {

// Runs the image's C initializers, then its C++ constructors. Returns false if
// a C initializer fails.
bool initializeImage();
// Executable start-up after the Universal CRT has its arguments and
// environment. Exits if a C initializer fails.
void initializeExecutable();
// DLL detach, after the user's DllMain: runs the image's registrations and its
// terminators.
void finalizeImage(bool Terminating);
// Reports a start-up failure and exits without running any terminator.
[[noreturn, gnu::cold]] void fatal(const char *Message);

struct ExitImage;

// One list of registrations: __cxa_atexit's, or __cxa_at_quick_exit's.
struct ExitRegistry {
  ExitImage *Images;  // every image's record, append-only
  ExitImage *Hint;    // the record found last
  ExitImage *Retired; // the records awaiting release
  LONG Token;         // whether the Universal CRT runs the list at exit
};

// The state of the termination registries, which every executable's entry
// point reserves and every copy of the registries in the process uses: the
// clang_rt.wincrt_dynamic.dll that most images import from, a second one
// loaded by full path or under another name, and the copy that -static links
// into an image. One list then orders every image's registrations, so exit
// runs them in exact reverse order across copies, under the one Universal CRT
// token the executable's start-up posts. A copy finds the state through
// PEB->ImageBaseAddress and the {ExitStateTag, RVA} that the linker writes at
// 0x30 in the executable's MZ header; without it, under a foreign executable,
// each copy keeps a state of its own. Every copy reads and writes it, the
// records and each thread's list, so any change to them changes
// ExitStateVersion.
struct ExitState {
  uint32_t Version;
  uint32_t Size;
  // One plus the TLS slot that holds a thread's thread-local destructors,
  // which the first registration of one allocates, so that an executable
  // without thread-locals needs no TLS directory.
  LONG ThreadSlot;
  // The key that registered functions are encoded with: the security cookie
  // of the first copy to register one, since each copy's differs.
  uintptr_t Cookie;
  ExitRegistry Normal, Quick;
  uint64_t Sequence;
  // Drainers keep the records of unloaded images readable, by epochs.
  uint32_t Epoch;
  LONG Drainers[2];
  LONG ExecutableRegistered;
  // 0 until a thread exits; then that thread's id, a multiple of 4, with bit 1
  // set while exit's drain runs and bit 0 once it has finished.
  LONG Drained;
};

constexpr uint32_t ExitStateTag = 0x7E1A5C03;
constexpr uint32_t ExitStateVersion = 1;

// The process's state: the executable's, or this copy's own.
ExitState &exitState();
// quick_exit under an executable that reserves the process's state, where
// every image's at_quick_exit registration is: completes the C termination
// and runs them, newest first. Returns false, having done nothing, under any
// other executable, whose registrations are in the Universal CRT's table.
bool runQuickExit();

using Destructor = void (*)(void *);

// A registered function is stored encoded, as the Universal CRT stores its own
// function tables, so that a write to the registries' memory cannot make the
// program call an address of the writer's choosing at exit.
inline uintptr_t exitCookie(ExitState &State) {
  uintptr_t Cookie = __atomic_load_n(&State.Cookie, __ATOMIC_ACQUIRE);
  if (!Cookie &&
      __atomic_compare_exchange_n(&State.Cookie, &Cookie, __security_cookie | 1,
                                  false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    Cookie = __security_cookie | 1;
  return Cookie;
}

inline uintptr_t encodeExit(ExitState &State, Destructor Function) {
  uintptr_t Cookie = exitCookie(State);
  return __builtin_rotateright64(reinterpret_cast<uintptr_t>(Function) ^ Cookie,
                                 Cookie & 63);
}

inline Destructor decodeExit(ExitState &State, uintptr_t Value) {
  uintptr_t Cookie = exitCookie(State);
  return reinterpret_cast<Destructor>(
      __builtin_rotateleft64(Value, Cookie & 63) ^ Cookie);
}

// Where functions carry type prefixes, the destructors that clang registers,
// through the __llvm_kcfi_ entry points, carry the type void(void *) salted
// "__cxa_dtor", and a call to one must use the same type. A function
// registered through the Itanium ABI's entry points has the plain type.
#if __has_feature(function_type_prefix)
#define WINCRT_DTOR_SALT __attribute__((cfi_salt("__cxa_dtor")))
#else
#define WINCRT_DTOR_SALT
#endif
typedef void (*SaltedDestructor)(void *) WINCRT_DTOR_SALT;

// The adaptor that registers a void() function, as atexit and at_quick_exit
// take one, as a salted destructor.
inline void callVoid(void *Function) WINCRT_DTOR_SALT {
  reinterpret_cast<void(__cdecl *)(void)>(Function)();
}

// Calls a registered destructor. Salted says whether it was registered as
// carrying the salted type.
inline void invokeCallback(Destructor Function, void *Object, bool Salted) {
  if (Salted)
    reinterpret_cast<SaltedDestructor>(Function)(Object);
  else
    Function(Object);
}

// The size of the image whose handle Module is.
inline uintptr_t imageSize(HMODULE Module) {
  auto *Dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(Module);
  return reinterpret_cast<const IMAGE_NT_HEADERS *>(
             reinterpret_cast<const char *>(Dos) + Dos->e_lfanew)
      ->OptionalHeader.SizeOfImage;
}

// Asks kernelbase.dll, which every Win32 process has loaded, one of the app
// model's policies. The Universal CRT asks through an API set whose host,
// kernel.appcore.dll, it loads for the purpose together with msvcrt.dll, and
// an import of the query would bind to that API set whenever a library named
// before kernel32.lib offers it. The Universal CRT asks nothing in a secure
// (IUM) process, whose process parameters carry the top bit of the flags
// that winternl.h leaves unnamed at offset 8. The call goes to a function of
// the system, which has no KCFI type.
template <typename Policy>
__attribute__((no_sanitize("kcfi"))) Policy appPolicy(const char *Query,
                                                      Policy Default) {
  auto *Parameters = reinterpret_cast<const ULONG *>(
      NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters);
  if (!(Parameters[2] & 0x80000000))
    if (auto Function = reinterpret_cast<LONG(WINAPI *)(HANDLE, Policy *)>(
            GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), Query)))
      Function(GetCurrentThreadEffectiveToken(), &Default);
  return Default;
}

inline void *crtAlloc(size_t Size) {
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

inline void crtFree(void *Memory) { HeapFree(GetProcessHeap(), 0, Memory); }

} // namespace wincrt

#endif // COMPILER_RT_LIB_WINCRT_WINCRT_H
