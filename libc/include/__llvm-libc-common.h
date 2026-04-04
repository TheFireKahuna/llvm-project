//===-- Common definitions for LLVM-libc public header files --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LLVM_LIBC_COMMON_H
#define _LLVM_LIBC_COMMON_H

#define __LLVM_LIBC__ 1

#ifdef __cplusplus

#undef __BEGIN_C_DECLS
#define __BEGIN_C_DECLS extern "C" {

#undef __END_C_DECLS
#define __END_C_DECLS }

// Standard C++ doesn't have C99 restrict but GNU C++ has it with __ spelling.
#undef __restrict
#ifndef __GNUC__
#define __restrict
#endif

#undef _Noreturn
#define _Noreturn [[noreturn]]

#undef _Alignas
#define _Alignas alignas

#undef _Static_assert
#define _Static_assert static_assert

#undef _Alignof
#define _Alignof alignof

#undef __NOEXCEPT
#if __cplusplus >= 201103L
#define __NOEXCEPT noexcept
#else
#define __NOEXCEPT throw()
#endif

#undef _Returns_twice
#if __cplusplus >= 201103L
#define _Returns_twice [[gnu::returns_twice]]
#else
#define _Returns_twice __attribute__((returns_twice))
#endif

// This macro serves as a generic cast implementation for use in both C and C++,
// similar to `__BIONIC_CAST` in Android.
#undef __LLVM_LIBC_CAST
#define __LLVM_LIBC_CAST(cast, type, value) (cast<type>(value))

#else // not __cplusplus

#undef __BEGIN_C_DECLS
#define __BEGIN_C_DECLS

#undef __END_C_DECLS
#define __END_C_DECLS

#undef __restrict
#if __STDC_VERSION__ >= 199901L
// C99 and above support the restrict keyword.
#define __restrict restrict
#elif !defined(__GNUC__)
// GNU-compatible compilers accept the __ spelling in all modes.
// Otherwise, omit the qualifier for pure C89 compatibility.
#define __restrict
#endif

#undef _Noreturn
#if __STDC_VERSION__ >= 201112L
// In C11 and later, _Noreturn is a keyword.
#elif defined(__GNUC__)
// GNU-compatible compilers have an equivalent attribute.
#define _Noreturn __attribute__((__noreturn__))
#else
#define _Noreturn
#endif

#undef __NOEXCEPT
#ifdef __GNUC__
#define __NOEXCEPT __attribute__((__nothrow__))
#else
#define __NOEXCEPT
#endif

#undef _Returns_twice
#define _Returns_twice __attribute__((returns_twice))

#undef __LLVM_LIBC_CAST
#define __LLVM_LIBC_CAST(cast, type, value) ((type)(value))

#endif // __cplusplus

// Import annotations for symbols provided by the libc shared library.
//
// On Windows, consumers of c.dll must see a dllimport attribute on exported
// symbols. For data (stdin/stdout/stderr/environ/...) omitting the attribute
// is a correctness bug: the compiler emits .refptr.<sym> with an ADDR64 reloc
// against <sym>, and the linker satisfies it with a private zero-initialized
// slot in the EXE's .data instead of indirecting through c.dll's IAT, so the
// consumer reads a stale null rather than c.dll's runtime-initialized value.
// For functions it is a QoI issue: without dllimport, calls go through an
// auto-generated trampoline instead of the IAT.
//
// Three build modes:
//   LIBC_FULL_BUILD      — TU is compiled into libc (static archive or DLL).
//                          Both macros expand to nothing; the definitions
//                          are locally visible or exported via c.def.
//   _LIBC_DLL (consumer) — consumer links against c.dll. Macros expand to
//                          a dllimport attribute. Set by the NTPOSIX driver
//                          by default; stripped under -static-libc.
//   Neither              — consumer statically links libc.lib. Macros expand
//                          to nothing; symbol is satisfied from the archive.
#if (defined(_WIN32) || defined(__CYGWIN__)) && !defined(LIBC_FULL_BUILD) && \
    defined(_LIBC_DLL)
#  if defined(__cplusplus) || \
      (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#    define __LIBC_DLLIMPORT_ATTR [[gnu::dllimport]]
#  else
#    define __LIBC_DLLIMPORT_ATTR __attribute__((dllimport))
#  endif
#else
#  define __LIBC_DLLIMPORT_ATTR
#endif

#ifndef __LIBC_DATA_IMPORT
#  define __LIBC_DATA_IMPORT __LIBC_DLLIMPORT_ATTR
#endif

#ifndef __LIBC_FUNC_IMPORT
#  define __LIBC_FUNC_IMPORT __LIBC_DLLIMPORT_ATTR
#endif

// Import annotation for symbols provided by external system DLLs (ntdll,
// bcryptprimitives, combase, sspicli, ucrtbase). These targets are always
// DLL-resident regardless of how libc itself is built, so the attribute is
// unconditionally applied on Windows — unlike __LIBC_DLLIMPORT_ATTR above,
// which only fires for consumers of c.dll. Using this macro (rather than a
// raw __declspec) keeps NT-header prototypes buildable without -fms-extensions.
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(__cplusplus)
#    define __LIBC_EXTERN_DLLIMPORT_ATTR [[gnu::dllimport]]
#  else
#    define __LIBC_EXTERN_DLLIMPORT_ATTR __attribute__((dllimport))
#  endif
#else
#  define __LIBC_EXTERN_DLLIMPORT_ATTR
#endif

// Export annotation for symbols that must be named-exports from a libc DSO —
// PE entry points (_DllMainCRTStartup) and per-image beacons looked up via
// LdrGetProcedureAddress (__libc_module_block). Most libc entrypoints do NOT
// use this; their exports flow through the generated c.def (see
// generate_libc_entrypoints_def in LLVMLibCLibraryRules.cmake). This macro
// exists to replace raw __declspec(dllexport) at the few sites that genuinely
// need a per-symbol export attribute, keeping the TU buildable without
// -fms-extensions.
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(__cplusplus)
#    define __LIBC_DLLEXPORT_ATTR [[gnu::dllexport]]
#  else
#    define __LIBC_DLLEXPORT_ATTR __attribute__((dllexport))
#  endif
#else
#  define __LIBC_DLLEXPORT_ATTR
#endif

// COMDAT "pick any" annotation for Windows startup globals that may be
// emitted by multiple TUs (CRT section bookends, security cookie, guard
// tables, __dso_handle, _fltused, etc.). The GNU/Clang `selectany` attribute
// maps to COFF IMAGE_COMDAT_SELECT_ANY semantics — identical duplicates are
// merged by the linker. Used in place of raw __LIBC_SELECTANY_ATTR to keep
// TUs buildable without -fms-extensions.
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(__cplusplus)
#    define __LIBC_SELECTANY_ATTR [[gnu::selectany]]
#  else
#    define __LIBC_SELECTANY_ATTR __attribute__((selectany))
#  endif
#else
#  define __LIBC_SELECTANY_ATTR
#endif

// Section placement for Windows startup bookends, CRT$XI/XC/XP/XT slots,
// per-image registry chunks (.libcveh / .libcfin / .libclzr / .pcb),
// CFGuard (.00cfg), RTTI seal (.rdata$ti[az]), and TLS directory entries
// (.tls / .CRT$XD[AZ] / .CRT$XL[ABCZ]). The GNU/Clang `section` attribute
// is the cross-platform equivalent of MSVC `__declspec(allocate(name))`.
// Using this macro keeps TUs buildable without -fms-extensions.
#if defined(__cplusplus)
#  define __LIBC_SECTION_ATTR(name) [[gnu::section(name)]]
#else
#  define __LIBC_SECTION_ATTR(name) __attribute__((section(name)))
#endif

// Microsoft x64 calling-convention attribute for functions that are invoked
// by the NT kernel or loader (TLS callbacks, DllMain, _DllMainCRTStartup,
// VEH handlers, APC routines, exception personalities). The loader places
// arguments in rcx/rdx/r8/r9 and reserves 32-byte shadow space per MS x64
// ABI; a callee compiled for a different default ABI (e.g. Itanium SysV)
// would misread its arguments and corrupt the frame.
//
// Empty on Windows ARM64 because Windows ARM64 uses AAPCS64, identical to
// the platform default. Empty on non-Windows targets.
//
// `ms_abi` is a function-type attribute. For a function declaration or
// definition the macro appears at the start of the declaration. To
// obtain a function *type* carrying the attribute (for callback typedefs),
// declare a prototype with LIBC_MSABI and alias its `decltype`:
//     LIBC_MSABI void Fn_proto(int);
//     using Fn = decltype(Fn_proto);
//     using Pfn = Fn *;
// Do *not* place the macro in the trailing position of a type alias
// (`using X = void(int) LIBC_MSABI;`) — GCC rejects `[[gnu::ms_abi]]`
// on a type, and clang flags it via -Wgcc-compat.
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#  if defined(__cplusplus) || \
      (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#    define LIBC_MSABI [[gnu::ms_abi]]
#  else
#    define LIBC_MSABI __attribute__((ms_abi))
#  endif
#else
#  define LIBC_MSABI
#endif

#endif // _LLVM_LIBC_COMMON_H
