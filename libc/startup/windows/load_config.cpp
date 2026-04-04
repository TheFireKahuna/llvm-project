//===-- load_config.cpp - PE Load Configuration Directory -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// IMAGE_LOAD_CONFIG_DIRECTORY for security cookie, CFG, XFG, CastGuard,
// EH continuation, and related PE security features. The Windows loader
// reads this to locate and enforce security infrastructure.
//
// Supports: x86, x86_64, AArch64, ARM64EC.
//
//===----------------------------------------------------------------------===//

#include "include/__llvm-libc-common.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

using DWORD = unsigned long;
using WORD = unsigned short;
using ULONGLONG = unsigned long long;
using VA = const volatile void *;

namespace {
// Keep in sync with kGuardFlags in cfguard.cpp. The load-config GuardFlags
// field is what the loader actually reads; __guard_flags is informational.
constexpr DWORD kLoadConfigGuardFlags =
    0x00000100UL |  // CF_INSTRUMENTED
    0x00000400UL |  // CF_FUNCTION_TABLE_PRESENT
    0x00010000UL |  // CF_LONGJUMP_TABLE_PRESENT
    0x00400000UL |  // EH_CONTINUATION_TABLE_PRESENT
    0x01000000UL;   // CASTGUARD_PRESENT
// TODO: emit a real cast guard table. CASTGUARD_PRESENT advertises that
// the failure-handler pointer is wired up, but the loader cannot enforce
// type checks at vptr loads without the per-class cast metadata table
// that MSVC's /CASTGUARD emits. Until clang grows the codegen pass and
// we add the linker-synthesised section, this flag is a half-promise:
// the runtime hook works, but no cast sites are instrumented.
} // namespace

//===----------------------------------------------------------------------===//
// External symbol declarations
//===----------------------------------------------------------------------===//

// Security cookie (security_cookie.cpp).
extern uintptr_t __security_cookie;

// CFG slot typedefs — must match cfguard.cpp definitions. Using typed
// function pointers (rather than `void *`) lets the defining TU
// initialize the slots from bare function names (constant expressions),
// so `.00cfg` registers as read-only without a `#pragma section`.
//
// CFG check/dispatch functions are invoked with MS x64 ABI — either by
// compiler-emitted indirect-call check sequences or by ntdll's
// LdrpValidateUserCallTarget when the slots are hotpatched by the loader.
//
// LIBC_MSABI decorates function *declarations* legally but not function
// *types* (trailing-alias form is rejected by GCC, warned by Clang under
// `-Wgcc-compat`). We therefore synthesise the pointer types via
// `decltype` on unreferenced per-arch prototypes: `static` gives them
// internal linkage, `decltype` is unevaluated, so no symbol is emitted.
#if defined(__x86_64__) || defined(__arm64ec__)
LIBC_MSABI void __cfg_check_type_source(uintptr_t);
LIBC_MSABI void __cfg_dispatch_type_source();
#elif defined(__aarch64__) && !defined(__arm64ec__)
void __cfg_check_type_source();
LIBC_MSABI void __cfg_dispatch_type_source();
#elif defined(__arm__)
void __cfg_check_type_source(uintptr_t);
void __cfg_dispatch_type_source();
#endif
using CfgCheckFn = decltype(&__cfg_check_type_source);
using CfgDispatchFn = decltype(&__cfg_dispatch_type_source);

extern "C" {

// CFG function pointers (cfguard.cpp).
extern const volatile CfgCheckFn __guard_check_icall_fptr;
extern const volatile CfgDispatchFn __guard_dispatch_icall_fptr;

// XFG function pointers (cfguard.cpp).
extern const volatile CfgCheckFn __guard_xfg_check_icall_fptr;
extern const volatile CfgDispatchFn __guard_xfg_dispatch_icall_fptr;
extern const volatile CfgDispatchFn __guard_xfg_table_dispatch_icall_fptr;

// CastGuard handler (cfguard.cpp). Invoked by compiler-emitted CastGuard
// failure dispatch with MS x64 ABI. Same `decltype`-on-prototype pattern
// as CfgCheck/CfgDispatch above.
LIBC_MSABI void __castguard_type_source(void *);
using CastGuardHandler = decltype(&__castguard_type_source);
extern const volatile CastGuardHandler __castguard_check_failure_os_handled_fptr;

// __guard_memcpy_fptr intentionally not declared: we don't claim
// CF_MEMCPY_FUNCTION_POINTER in __guard_flags, and the only symbol the
// linker would resolve was an empty fallback. The
// GuardMemcpyFunctionPointer field is left null below.

} // extern "C"

//===----------------------------------------------------------------------===//
// Fallback symbols for optional linker-generated tables
//
// Each CFG / enclave / volatile-metadata symbol is emitted as a COFF weak
// external aliased to a one-byte empty fallback. When lld-link's
// /GUARD:CF, /GUARD:EHCONT, or /ENCLAVE pass synthesizes a strong
// definition, the strong def wins over the weak alias (verified under
// IMAGE_WEAK_EXTERN_SEARCH_ALIAS). When the pass is inactive, the alias
// resolves to the empty fallback and the load config entry points at a
// valid (empty) buffer.
//===----------------------------------------------------------------------===//

extern "C" {

__LIBC_SELECTANY_ATTR char __guard_fids_table_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_fids_count_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_iat_table_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_iat_count_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_longjmp_table_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_longjmp_count_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_eh_cont_table_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __guard_eh_cont_count_empty__[1] = {0};

__LIBC_SELECTANY_ATTR char __enclave_config_empty__[1] = {0};
__LIBC_SELECTANY_ATTR char __volatile_metadata_empty__[1] = {0};

__attribute__((weak, alias("__guard_fids_table_empty__")))
extern char __guard_fids_table[1];
__attribute__((weak, alias("__guard_fids_count_empty__")))
extern char __guard_fids_count[1];
__attribute__((weak, alias("__guard_iat_table_empty__")))
extern char __guard_iat_table[1];
__attribute__((weak, alias("__guard_iat_count_empty__")))
extern char __guard_iat_count[1];
__attribute__((weak, alias("__guard_longjmp_table_empty__")))
extern char __guard_longjmp_table[1];
__attribute__((weak, alias("__guard_longjmp_count_empty__")))
extern char __guard_longjmp_count[1];
__attribute__((weak, alias("__guard_eh_cont_table_empty__")))
extern char __guard_eh_cont_table[1];
__attribute__((weak, alias("__guard_eh_cont_count_empty__")))
extern char __guard_eh_cont_count[1];

__attribute__((weak, alias("__enclave_config_empty__")))
extern char __enclave_config[1];
__attribute__((weak, alias("__volatile_metadata_empty__")))
extern char __volatile_metadata[1];

} // extern "C"

//===----------------------------------------------------------------------===//
// Code integrity embedded struct
//===----------------------------------------------------------------------===//

struct CodeIntegrity {
  WORD Flags;
  WORD Catalog;
  DWORD CatalogOffset;
  DWORD Reserved;
};

static_assert(sizeof(CodeIntegrity) == 12, "size mismatch");

//===----------------------------------------------------------------------===//
// 64-bit Load Configuration Directory
//===----------------------------------------------------------------------===//

#if defined(__x86_64__) || defined(__aarch64__)

#if defined(__arm64ec__)
struct ChpeMetadata {
  DWORD Version;
  DWORD Fields[22];
};
static_assert(sizeof(ChpeMetadata) == 0x5C, "size mismatch");
extern "C" ChpeMetadata __chpe_metadata;
#endif

struct LoadConfig64 {
  DWORD Size;
  DWORD TimeDateStamp;
  WORD MajorVersion;
  WORD MinorVersion;
  DWORD GlobalFlagsClear;
  DWORD GlobalFlagsSet;
  DWORD CriticalSectionDefaultTimeout;
  ULONGLONG DeCommitFreeBlockThreshold;
  ULONGLONG DeCommitTotalFreeThreshold;
  VA LockPrefixTable;
  ULONGLONG MaximumAllocationSize;
  ULONGLONG VirtualMemoryThreshold;
  ULONGLONG ProcessAffinityMask;
  DWORD ProcessHeapFlags;
  WORD CSDVersion;
  WORD DependentLoadFlags;
  VA EditList;
  VA SecurityCookie;
  VA SEHandlerTable;
  VA SEHandlerCount;
  VA GuardCFCheckFunctionPointer;
  VA GuardCFDispatchFunctionPointer;
  VA GuardCFFunctionTable;
  VA GuardCFFunctionCount;
  DWORD GuardFlags;
  CodeIntegrity CI;
  VA GuardAddressTakenIatEntryTable;
  VA GuardAddressTakenIatEntryCount;
  VA GuardLongJumpTargetTable;
  VA GuardLongJumpTargetCount;
  VA DynamicValueRelocTable;
  VA CHPEMetadataPointer;
  VA GuardRFFailureRoutine;
  VA GuardRFFailureRoutineFunctionPointer;
  DWORD DynamicValueRelocTableOffset;
  WORD DynamicValueRelocTableSection;
  WORD Reserved2;
  VA GuardRFVerifyStackPointerFunctionPointer;
  DWORD HotPatchTableOffset;
  DWORD Reserved3;
  VA EnclaveConfigurationPointer;
  VA VolatileMetadataPointer;
  VA GuardEHContinuationTable;
  VA GuardEHContinuationCount;
  VA GuardXFGCheckFunctionPointer;
  VA GuardXFGDispatchFunctionPointer;
  VA GuardXFGTableDispatchFunctionPointer;
  VA CastGuardOsDeterminedFailureMode;
  VA GuardMemcpyFunctionPointer;
  VA UmaFunctionPointers;
};

static_assert(sizeof(VA) == sizeof(ULONGLONG), "pointer size mismatch");
static_assert(sizeof(LoadConfig64) == 0x148, "size mismatch");
static_assert(offsetof(LoadConfig64, SecurityCookie) == 0x58,
              "SecurityCookie offset mismatch");
static_assert(offsetof(LoadConfig64, GuardFlags) == 0x90,
              "GuardFlags offset mismatch");
static_assert(offsetof(LoadConfig64, EnclaveConfigurationPointer) == 0xF8,
              "EnclaveConfigurationPointer offset mismatch");

// `_load_config_used` below is `const`, so clang registers .rdata$T as
// PSF_Read without any #pragma section declaration.

// DependentLoadFlags narrows implicit DLL resolution. That is a good default
// for the Win32/UCRT personality, but NT-POSIX developer workflows routinely
// run test executables from nested output directories while staging c.dll and
// unwind.dll in a parent runtime directory. LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
// excludes both the current working directory and PATH, which makes that
// standard app-local layout unloadable before main() ever runs.
//
// Keep the stricter default for non-NTPOSIX Windows targets, but let NTPOSIX
// use the platform's normal search semantics so external test suites and
// greenfield app-local runtime layouts work without per-directory DLL copies.
#ifdef __NTPOSIX__
constexpr WORD kDependentLoadFlags = 0;
#else
constexpr WORD kDependentLoadFlags = 0x1000; // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#endif

constexpr LoadConfig64 make_load_config64() {
  LoadConfig64 cfg = {};
  cfg.Size = sizeof(LoadConfig64);
  cfg.DependentLoadFlags = kDependentLoadFlags;
  cfg.SecurityCookie = &__security_cookie;
  cfg.GuardCFCheckFunctionPointer = &__guard_check_icall_fptr;
  cfg.GuardCFDispatchFunctionPointer = &__guard_dispatch_icall_fptr;
  cfg.GuardCFFunctionTable = &__guard_fids_table;
  cfg.GuardCFFunctionCount = &__guard_fids_count;
  cfg.GuardFlags = kLoadConfigGuardFlags;
  cfg.GuardAddressTakenIatEntryTable = &__guard_iat_table;
  cfg.GuardAddressTakenIatEntryCount = &__guard_iat_count;
  cfg.GuardLongJumpTargetTable = &__guard_longjmp_table;
  cfg.GuardLongJumpTargetCount = &__guard_longjmp_count;
#if defined(__arm64ec__)
  cfg.CHPEMetadataPointer = &__chpe_metadata;
#endif
  cfg.EnclaveConfigurationPointer = &__enclave_config;
  cfg.VolatileMetadataPointer = &__volatile_metadata;
  cfg.GuardEHContinuationTable = &__guard_eh_cont_table;
  cfg.GuardEHContinuationCount = &__guard_eh_cont_count;
  cfg.GuardXFGCheckFunctionPointer = &__guard_xfg_check_icall_fptr;
  cfg.GuardXFGDispatchFunctionPointer = &__guard_xfg_dispatch_icall_fptr;
  cfg.GuardXFGTableDispatchFunctionPointer =
      &__guard_xfg_table_dispatch_icall_fptr;
  cfg.CastGuardOsDeterminedFailureMode =
      &__castguard_check_failure_os_handled_fptr;
  // Everything else intentionally remains zero-initialized.
  return cfg;
}

// `[[gnu::retain]]` emits /INCLUDE: into `.drectve` so the PE loader's
// load config directory always resolves.
extern "C" [[gnu::retain]] __LIBC_SECTION_ATTR(".rdata$T") const LoadConfig64
    _load_config_used = make_load_config64();

#endif // __x86_64__ || __aarch64__
