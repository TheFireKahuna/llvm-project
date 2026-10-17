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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <winternl.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "wincrt supports x86-64 and AArch64 only"
#endif

#define WINCRT_STRINGIFY(Name) #Name
#define WINCRT_ALTERNATENAME(From, To)                                         \
  __pragma(comment(linker, "/alternatename:" WINCRT_STRINGIFY(                 \
                               From) "=" WINCRT_STRINGIFY(To)))
#define WINCRT_INCLUDE(Name)                                                   \
  __pragma(comment(linker, "/include:" WINCRT_STRINGIFY(Name)))

// Every entry object wraps the Universal CRT functions that wincrt defines
// again, so that each reference to them in the image, the Universal CRT's
// import included, reaches wincrt's definitions.
#define WINCRT_WRAP_UCRT                                                       \
  __pragma(comment(linker,                                                     \
                   "/wrap:exit /wrap:_exit /wrap:_Exit "                       \
                   "/wrap:_beginthread /wrap:_beginthreadex "                  \
                   "/wrap:_endthread /wrap:_endthreadex /wrap:rand_s "         \
                   "/wrap:raise /wrap:abort"))

// The parts of wincrt that serve the whole process: the termination
// registries, exit, raise, abort, the thread start and rand_s. Every image
// imports them from clang_rt.wincrt_dynamic.dll, which exports them, so that
// registration is process-wide and finalization per image; a program linked
// with -static keeps them in the executable, from clang_rt.wincrt_static.lib.
#ifdef COMPILER_RT_SHARED_LIB
#define WINCRT_ATEXIT_API __attribute__((visibility("default")))
#else
#define WINCRT_ATEXIT_API
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

WINCRT_ATEXIT_API int __cdecl __cxa_atexit(void (*)(void *), void *, void *);
WINCRT_ATEXIT_API int __cdecl __llvm_kcfi_cxa_atexit(void (*)(void *), void *,
                                                     void *);
WINCRT_ATEXIT_API void __cdecl __cxa_finalize(void *);
WINCRT_ATEXIT_API int __cxa_at_quick_exit(void (*)(void), void *);
WINCRT_ATEXIT_API int __cxa_thread_atexit_impl(void (*)(void *), void *,
                                               void *);
WINCRT_ATEXIT_API int __llvm_kcfi_cxa_thread_atexit_impl(void (*)(void *),
                                                         void *, void *);
// Runs the calling thread's thread-local destructors: those of one image,
// or all of them, completing the thread, if the argument is null.
WINCRT_ATEXIT_API void __cxa_thread_finalize(void *);
// The executable's start-up makes the registry live for the whole process
// and hands it the executable's terminators, which run after its drain.
WINCRT_ATEXIT_API void __wincrt_register_executable(void (*)(void));
// A DLL's detach runs its registrations, unless the process is terminating
// without having run them. Returns whether it ran them.
WINCRT_ATEXIT_API int __wincrt_detach_image(void *, int);
}

// An image without libc++abi has no active exception to report. The alias
// names wincrt's abort itself, since the wrap of abort renames references to
// it but not the target of an alias.
WINCRT_ALTERNATENAME(__cxa_call_terminate, __wrap_abort)

// IMAGE_LOAD_CONFIG_DIRECTORY64, but with GuardFlags and the CodeIntegrity
// flags and catalog, which are zero, as one 64-bit field. The guard flags are
// the value of an absolute symbol, and only a pointer-sized field can hold a
// symbol's value in a constant initializer. The linker applies no base
// relocation to an absolute symbol, and the flags fit in the low 32 bits.
struct LoadConfig {
  DWORD Size;
  DWORD TimeDateStamp;
  WORD MajorVersion;
  WORD MinorVersion;
  DWORD GlobalFlagsClear;
  DWORD GlobalFlagsSet;
  DWORD CriticalSectionDefaultTimeout;
  ULONGLONG DeCommitFreeBlockThreshold;
  ULONGLONG DeCommitTotalFreeThreshold;
  ULONGLONG LockPrefixTable;
  ULONGLONG MaximumAllocationSize;
  ULONGLONG VirtualMemoryThreshold;
  ULONGLONG ProcessAffinityMask;
  DWORD ProcessHeapFlags;
  WORD CSDVersion;
  WORD DependentLoadFlags;
  ULONGLONG EditList;
  ULONGLONG SecurityCookie;
  ULONGLONG SEHandlerTable;
  ULONGLONG SEHandlerCount;
  ULONGLONG GuardCFCheckFunctionPointer;
  ULONGLONG GuardCFDispatchFunctionPointer;
  ULONGLONG GuardCFFunctionTable;
  ULONGLONG GuardCFFunctionCount;
  ULONGLONG GuardFlagsAndCodeIntegrity;
  DWORD CodeIntegrityCatalogOffset;
  DWORD CodeIntegrityReserved;
  ULONGLONG GuardAddressTakenIatEntryTable;
  ULONGLONG GuardAddressTakenIatEntryCount;
  ULONGLONG GuardLongJumpTargetTable;
  ULONGLONG GuardLongJumpTargetCount;
  ULONGLONG DynamicValueRelocTable;
  ULONGLONG CHPEMetadataPointer;
  ULONGLONG GuardRFFailureRoutine;
  ULONGLONG GuardRFFailureRoutineFunctionPointer;
  DWORD DynamicValueRelocTableOffset;
  WORD DynamicValueRelocTableSection;
  WORD Reserved2;
  ULONGLONG GuardRFVerifyStackPointerFunctionPointer;
  DWORD HotPatchTableOffset;
  DWORD Reserved3;
  ULONGLONG EnclaveConfigurationPointer;
  ULONGLONG VolatileMetadataPointer;
  ULONGLONG GuardEHContinuationTable;
  ULONGLONG GuardEHContinuationCount;
  ULONGLONG GuardXFGCheckFunctionPointer;
  ULONGLONG GuardXFGDispatchFunctionPointer;
  ULONGLONG GuardXFGTableDispatchFunctionPointer;
  ULONGLONG CastGuardOsDeterminedFailureMode;
  ULONGLONG GuardMemcpyFunctionPointer;
  ULONGLONG UmaFunctionPointers;
};

static_assert(sizeof(LoadConfig) == sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64));
static_assert(offsetof(LoadConfig, DependentLoadFlags) ==
              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DependentLoadFlags));
static_assert(offsetof(LoadConfig, SecurityCookie) ==
              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie));
static_assert(offsetof(LoadConfig, GuardFlagsAndCodeIntegrity) ==
              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags));
static_assert(offsetof(LoadConfig, EnclaveConfigurationPointer) ==
              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,
                       EnclaveConfigurationPointer));
static_assert(offsetof(LoadConfig, GuardXFGCheckFunctionPointer) ==
              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,
                       GuardXFGCheckFunctionPointer));
static_assert(offsetof(LoadConfig, UmaFunctionPointers) ==
              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, UmaFunctionPointers));

// The image's load configuration, defined in loadconfig.cpp. The linker writes
// some of its fields after compilation, so code reads them through a volatile
// glvalue.
#pragma section(".rdata$T", read)
extern "C" __declspec(allocate(".rdata$T")) const LoadConfig _load_config_used;

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

// Whether Record is an Itanium exception raised by one of the unwinder's two
// functions that begin the search for a handler, linked into the executable
// or exported by the image that raised it.
bool raisedByUnwinder(const EXCEPTION_RECORD *Record);

// An Itanium exception that reaches a frame it must not pass terminates the
// program there, before anything is unwound, so that std::terminate sees it
// as the active exception with the thrower's frames intact. Every other
// exception goes on to the next handler. The registries' DLL links no C++
// runtime, so there the raise resumes, and the thrower's runtime calls
// std::terminate as it does when no frame handles the exception.
inline int terminateFilter(EXCEPTION_POINTERS *Exception) {
  const EXCEPTION_RECORD *Record = Exception->ExceptionRecord;
#ifdef COMPILER_RT_SHARED_LIB
  if (raisedByUnwinder(Record))
    return EXCEPTION_CONTINUE_EXECUTION;
#else
  if (Record->ExceptionCode == GCCExceptionCode &&
      Record->NumberParameters == 1 && Record->ExceptionInformation[0])
    __cxa_call_terminate(
        reinterpret_cast<void *>(Record->ExceptionInformation[0]));
#endif
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

// Where functions carry type prefixes, the destructors that clang registers,
// through the __llvm_kcfi_ entry points, carry the type void(void *) salted
// "__cxa_dtor", whether or not kcfi checks calls, and a call to one must use
// the same type. A function registered through the Itanium ABI's entry points
// has the plain type.
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

// A registered destructor that exits by an exception terminates the program
// ([basic.start.term], [support.start.term]). Salted says whether Function
// was registered as carrying the salted type.
inline void invokeCallback(Destructor Function, void *Object, bool Salted) {
  __try {
    if (Salted)
      reinterpret_cast<SaltedDestructor>(Function)(Object);
    else
      Function(Object);
  } __except (terminateFilter(GetExceptionInformation())) {
    // The filter never selects this handler.
    __builtin_unreachable();
  }
}

// The size of the image whose handle Module is.
inline uintptr_t imageSize(HMODULE Module) {
  const auto *Dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(Module);
  return reinterpret_cast<const IMAGE_NT_HEADERS *>(
             reinterpret_cast<const char *>(Dos) + Dos->e_lfanew)
      ->OptionalHeader.SizeOfImage;
}

inline void *crtAlloc(size_t Size) {
  return HeapAlloc(GetProcessHeap(), 0, Size);
}

inline void crtFree(void *Memory) { HeapFree(GetProcessHeap(), 0, Memory); }

// Whether this is a secure (IUM) process, whose process parameters carry
// RTL_USER_PROC_SECURE_PROCESS, the top bit of the flags that winternl.h
// leaves unnamed at offset 8. The Universal CRT asks no app model policy
// there.
inline bool isSecureProcess() {
  const char *Parameters = reinterpret_cast<const char *>(
      NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters);
  ULONG Flags;
  __builtin_memcpy(&Flags, Parameters + 8, sizeof(Flags));
  return Flags & 0x80000000;
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
