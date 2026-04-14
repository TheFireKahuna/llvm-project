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

#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

using DWORD = unsigned long;
using WORD = unsigned short;
using ULONGLONG = unsigned long long;
using VA = const volatile void *;

namespace {
constexpr DWORD kLoadConfigGuardFlags = 0x00000100 | 0x00000400;
} // namespace

//===----------------------------------------------------------------------===//
// External symbol declarations
//===----------------------------------------------------------------------===//

// Security cookie (security_cookie.cpp).
extern uintptr_t __security_cookie;

extern "C" {

// CFG function pointers (cfguard.cpp).
extern volatile void *__guard_check_icall_fptr;
extern volatile void *__guard_dispatch_icall_fptr;

// XFG function pointers (cfguard.cpp).
extern volatile void *__guard_xfg_check_icall_fptr;
extern volatile void *__guard_xfg_dispatch_icall_fptr;
extern volatile void *__guard_xfg_table_dispatch_icall_fptr;

// CastGuard handler (cfguard.cpp).
using CastGuardHandler = void(__cdecl *)(void *);
extern volatile CastGuardHandler __castguard_check_failure_os_handled_fptr;

// Linker-generated CFG tables. Fallbacks via /alternatename below.
extern char __guard_fids_table[];
extern char __guard_fids_count[];
extern char __guard_iat_table[];
extern char __guard_iat_count[];
extern char __guard_longjmp_table[];
extern char __guard_longjmp_count[];
extern char __guard_eh_cont_table[];
extern char __guard_eh_cont_count[];
extern void *__guard_memcpy_fptr;

#if defined(__i386__)
extern char __safe_se_handler_table[];
extern char __safe_se_handler_count[];
#endif

extern char __enclave_config[];
extern char __volatile_metadata[];

} // extern "C"

//===----------------------------------------------------------------------===//
// Fallback symbols for optional linker-generated tables
//===----------------------------------------------------------------------===//

extern "C" {

__declspec(selectany) void *__guard_memcpy_fptr_empty__ = nullptr;
__declspec(selectany) char __guard_fids_table_empty__[1] = {0};
__declspec(selectany) char __guard_fids_count_empty__[1] = {0};
__declspec(selectany) char __guard_iat_table_empty__[1] = {0};
__declspec(selectany) char __guard_iat_count_empty__[1] = {0};
__declspec(selectany) char __guard_longjmp_table_empty__[1] = {0};
__declspec(selectany) char __guard_longjmp_count_empty__[1] = {0};
__declspec(selectany) char __guard_eh_cont_table_empty__[1] = {0};
__declspec(selectany) char __guard_eh_cont_count_empty__[1] = {0};

#if defined(__i386__)
__declspec(selectany) char __safe_se_handler_table_empty__[1] = {0};
__declspec(selectany) char __safe_se_handler_count_empty__[1] = {0};
#endif

__declspec(selectany) char __enclave_config_empty__[1] = {0};
__declspec(selectany) char __volatile_metadata_empty__[1] = {0};

} // extern "C"

// clang-format off

// /alternatename: use empty fallbacks when linker doesn't provide real tables.
#if defined(__i386__)
#define ALT_PREFIX "_"
#else
#define ALT_PREFIX ""
#endif

#define ALTERNATENAME(weak, strong)                                            \
  __pragma(comment(linker, "/alternatename:" ALT_PREFIX #weak "="              \
                                             ALT_PREFIX #strong))

ALTERNATENAME(__guard_memcpy_fptr, __guard_memcpy_fptr_empty__)
ALTERNATENAME(__guard_fids_table, __guard_fids_table_empty__)
ALTERNATENAME(__guard_fids_count, __guard_fids_count_empty__)
ALTERNATENAME(__guard_iat_table, __guard_iat_table_empty__)
ALTERNATENAME(__guard_iat_count, __guard_iat_count_empty__)
ALTERNATENAME(__guard_longjmp_table, __guard_longjmp_table_empty__)
ALTERNATENAME(__guard_longjmp_count, __guard_longjmp_count_empty__)
ALTERNATENAME(__guard_eh_cont_table, __guard_eh_cont_table_empty__)
ALTERNATENAME(__guard_eh_cont_count, __guard_eh_cont_count_empty__)

#if defined(__i386__)
ALTERNATENAME(__safe_se_handler_table, __safe_se_handler_table_empty__)
ALTERNATENAME(__safe_se_handler_count, __safe_se_handler_count_empty__)
#endif

ALTERNATENAME(__enclave_config, __enclave_config_empty__)
ALTERNATENAME(__volatile_metadata, __volatile_metadata_empty__)

// clang-format on

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

#pragma section(".rdata$T", long, read)

// clang-format off
__declspec(allocate(".rdata$T"))
extern "C" const LoadConfig64 _load_config_used = {
    sizeof(LoadConfig64),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    &__security_cookie,
    nullptr, nullptr,
    &__guard_check_icall_fptr,
    &__guard_dispatch_icall_fptr,
    &__guard_fids_table,
    &__guard_fids_count,
    kLoadConfigGuardFlags,
    {0, 0, 0, 0},
    &__guard_iat_table,
    &__guard_iat_count,
    &__guard_longjmp_table,
    &__guard_longjmp_count,
    nullptr,
#if defined(__arm64ec__)
    &__chpe_metadata,
#else
    nullptr,
#endif
    nullptr, nullptr, 0, 0, 0, nullptr,
    0, 0,
    &__enclave_config,
    &__volatile_metadata,
    &__guard_eh_cont_table,
    &__guard_eh_cont_count,
    &__guard_xfg_check_icall_fptr,
    &__guard_xfg_dispatch_icall_fptr,
    &__guard_xfg_table_dispatch_icall_fptr,
    &__castguard_check_failure_os_handled_fptr,
    &__guard_memcpy_fptr,
    nullptr,
};
// clang-format on

#endif // __x86_64__ || __aarch64__

//===----------------------------------------------------------------------===//
// 32-bit Load Configuration Directory
//===----------------------------------------------------------------------===//

#if defined(__i386__) || defined(__arm__)

struct LoadConfig32 {
  DWORD Size;
  DWORD TimeDateStamp;
  WORD MajorVersion;
  WORD MinorVersion;
  DWORD GlobalFlagsClear;
  DWORD GlobalFlagsSet;
  DWORD CriticalSectionDefaultTimeout;
  DWORD DeCommitFreeBlockThreshold;
  DWORD DeCommitTotalFreeThreshold;
  VA LockPrefixTable;
  DWORD MaximumAllocationSize;
  DWORD VirtualMemoryThreshold;
  DWORD ProcessAffinityMask;
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

static_assert(sizeof(VA) == sizeof(DWORD), "pointer size mismatch");
static_assert(sizeof(LoadConfig32) == 0xC4, "size mismatch");
static_assert(offsetof(LoadConfig32, SecurityCookie) == 0x3C,
              "SecurityCookie offset mismatch");
static_assert(offsetof(LoadConfig32, GuardFlags) == 0x58,
              "GuardFlags offset mismatch");
static_assert(offsetof(LoadConfig32, GuardXFGCheckFunctionPointer) == 0xAC,
              "GuardXFGCheckFunctionPointer offset mismatch");

#pragma section(".rdata$T", long, read)

// clang-format off
__declspec(allocate(".rdata$T"))
extern "C" const LoadConfig32 _load_config_used = {
    sizeof(LoadConfig32),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    &__security_cookie,
#if defined(__i386__)
    &__safe_se_handler_table,
    &__safe_se_handler_count,
#else
    nullptr, nullptr,
#endif
    &__guard_check_icall_fptr,
    nullptr, // GuardCFDispatchFunctionPointer (x64/ARM64 only)
    &__guard_fids_table,
    &__guard_fids_count,
    kLoadConfigGuardFlags,
    {0, 0, 0, 0},
    &__guard_iat_table,
    &__guard_iat_count,
    &__guard_longjmp_table,
    &__guard_longjmp_count,
    nullptr, nullptr,
    nullptr, nullptr, 0, 0, 0, nullptr,
    0, 0,
    &__enclave_config,
    &__volatile_metadata,
    &__guard_eh_cont_table,
    &__guard_eh_cont_count,
    &__guard_xfg_check_icall_fptr,
    &__guard_xfg_dispatch_icall_fptr,
    &__guard_xfg_table_dispatch_icall_fptr,
    &__castguard_check_failure_os_handled_fptr,
    &__guard_memcpy_fptr,
    nullptr,
};
// clang-format on

#endif // __i386__ || __arm__

// Force linker to include _load_config_used.
#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:__load_config_used")
#else
#pragma comment(linker, "/INCLUDE:_load_config_used")
#endif
