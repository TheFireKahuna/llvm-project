//===-- loadconfig.cpp - PE Load Configuration Directory -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PE Load Config Directory for security cookie, CFG, and related features.
//
// This file provides the IMAGE_LOAD_CONFIG_DIRECTORY structure that Windows
// uses to locate security features. All linker-generated symbols have fallback
// definitions via /alternatename to support builds without specific features.
//
// Supported features (Windows 10 20H1+ / build 19041+):
//   - /GS security cookie
//   - /GUARD:CF (Control Flow Guard)
//   - /GUARD:EHcont (Exception Handler Continuation)
//   - XFG (eXtended Flow Guard) - Windows 11+
//   - CastGuard - Windows 11+
//   - SafeSEH (x86 only)
//   - Enclave configuration
//   - Volatile metadata (Spectre mitigations)
//
// MUTUAL EXCLUSIVITY: Defines _load_config_used which conflicts with vcruntime
// or linker-generated versions. See init.cpp for enforcement mechanism.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "internal.h"

// Structures sized to match SDK 10.0.26100.0 IMAGE_LOAD_CONFIG_DIRECTORY.
using ULONGLONG = unsigned __int64;

// CastGuard handler type.
using _castguard_handler = void(__cdecl *)(void *);

//===----------------------------------------------------------------------===//
// External symbol declarations
//===----------------------------------------------------------------------===//

// Security cookie has C++ linkage to match SDK vcruntime.h.
extern uintptr_t __security_cookie;

extern "C" {

// CFG function pointers (cfguard.cpp).
extern volatile void *__guard_check_icall_fptr;
extern volatile void *__guard_dispatch_icall_fptr;
extern DWORD __guard_flags;

// XFG function pointers (cfguard.cpp).
extern volatile void *__guard_xfg_check_icall_fptr;
extern volatile void *__guard_xfg_dispatch_icall_fptr;
extern volatile void *__guard_xfg_table_dispatch_icall_fptr;

// CastGuard handler (cfguard.cpp).
extern volatile _castguard_handler __castguard_check_failure_os_handled_fptr;

// Linker-generated CFG tables.
extern char __guard_fids_table[];
extern char __guard_fids_count[];
extern char __guard_iat_table[];
extern char __guard_iat_count[];
extern char __guard_longjmp_table[];
extern char __guard_longjmp_count[];
extern char __guard_eh_cont_table[];
extern char __guard_eh_cont_count[];

// GuardMemcpy - secure memcpy for CFG (linker-generated when enabled).
extern void *__guard_memcpy_fptr;

// SafeSEH tables (x86 only, linker-generated with /SAFESEH).
#if defined(__i386__)
extern char __safe_se_handler_table[];
extern char __safe_se_handler_count[];
#endif

// Enclave configuration (linker-generated with /ENCLAVE).
extern char __enclave_config[];

// Volatile metadata for Spectre mitigations (linker-generated).
extern char __volatile_metadata[];

} // extern "C"

//===----------------------------------------------------------------------===//
// Fallback symbols for optional linker-generated tables
//===----------------------------------------------------------------------===//
//
// These provide empty fallbacks when linking without specific security features.
// The linker's /alternatename directive selects these when the real symbols
// aren't provided by the linker.
//

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

// CFG tables.
WINCRT_ALTERNATENAME(__guard_memcpy_fptr, __guard_memcpy_fptr_empty__)
WINCRT_ALTERNATENAME(__guard_fids_table, __guard_fids_table_empty__)
WINCRT_ALTERNATENAME(__guard_fids_count, __guard_fids_count_empty__)
WINCRT_ALTERNATENAME(__guard_iat_table, __guard_iat_table_empty__)
WINCRT_ALTERNATENAME(__guard_iat_count, __guard_iat_count_empty__)
WINCRT_ALTERNATENAME(__guard_longjmp_table, __guard_longjmp_table_empty__)
WINCRT_ALTERNATENAME(__guard_longjmp_count, __guard_longjmp_count_empty__)
WINCRT_ALTERNATENAME(__guard_eh_cont_table, __guard_eh_cont_table_empty__)
WINCRT_ALTERNATENAME(__guard_eh_cont_count, __guard_eh_cont_count_empty__)

// SafeSEH (x86 only).
#if defined(__i386__)
WINCRT_ALTERNATENAME(__safe_se_handler_table, __safe_se_handler_table_empty__)
WINCRT_ALTERNATENAME(__safe_se_handler_count, __safe_se_handler_count_empty__)
#endif

// Enclave and volatile metadata.
WINCRT_ALTERNATENAME(__enclave_config, __enclave_config_empty__)
WINCRT_ALTERNATENAME(__volatile_metadata, __volatile_metadata_empty__)

//===----------------------------------------------------------------------===//
// Embedded structures
//===----------------------------------------------------------------------===//

struct wincrt_code_integrity {
  WORD Flags;
  WORD Catalog;
  DWORD CatalogOffset;
  DWORD Reserved;
};

static_assert(sizeof(wincrt_code_integrity) == 12, "size mismatch");

//===----------------------------------------------------------------------===//
// ARM64EC CHPE metadata
//===----------------------------------------------------------------------===//

struct wincrt_chpe_metadata {
  DWORD Version;
  DWORD CodeMap;
  DWORD CodeMapCount;
  DWORD CodeRangesToEntryPoints;
  DWORD RedirectionMetadata;
  DWORD __os_arm64x_dispatch_call_no_redirect;
  DWORD __os_arm64x_dispatch_ret;
  DWORD __os_arm64x_dispatch_call;
  DWORD __os_arm64x_dispatch_icall;
  DWORD __os_arm64x_dispatch_icall_cfg;
  DWORD AlternateEntryPoint;
  DWORD AuxiliaryIAT;
  DWORD CodeRangesToEntryPointsCount;
  DWORD RedirectionMetadataCount;
  DWORD GetX64InformationFunctionPointer;
  DWORD SetX64InformationFunctionPointer;
  DWORD ExtraRFETable;
  DWORD ExtraRFETableSize;
  DWORD __os_arm64x_dispatch_fptr;
  DWORD AuxiliaryIATCopy;
  DWORD AuxiliaryDelayloadIAT;
  DWORD AuxiliaryDelayloadIATCopy;
  DWORD HybridImageInfoBitfield;
};

static_assert(sizeof(wincrt_chpe_metadata) == 0x5C, "size mismatch");

#if defined(__arm64ec__)

extern "C" {
extern DWORD __hybrid_code_map;
extern DWORD __hybrid_code_map_count;
extern DWORD __x64_code_ranges_to_entry_points;
extern DWORD __x64_code_ranges_to_entry_points_count;
extern DWORD __arm64x_redirection_metadata;
extern DWORD __arm64x_redirection_metadata_count;
extern DWORD __hybrid_auxiliary_iat;
extern DWORD __hybrid_auxiliary_iat_copy;
extern DWORD __hybrid_auxiliary_delayload_iat;
extern DWORD __hybrid_auxiliary_delayload_iat_copy;
extern DWORD __hybrid_image_info_bitfield;
}

#pragma section(".rdata$CHPE", long, read)

__declspec(allocate(".rdata$CHPE")) extern "C" wincrt_chpe_metadata
    __chpe_metadata = {
        2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#pragma comment(linker, "/INCLUDE:__chpe_metadata")

#endif // __arm64ec__

#if defined(__arm64ec__)
extern "C" wincrt_chpe_metadata __chpe_metadata;
#endif

//===----------------------------------------------------------------------===//
// 64-bit Load Configuration Directory (x64, ARM64, ARM64EC)
//===----------------------------------------------------------------------===//

#if defined(__x86_64__) || defined(__aarch64__)

struct wincrt_load_config64 {
  // Version 1 fields (Windows XP).
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

  // Version 2 fields (Windows Vista /GS).
  ULONGLONG SecurityCookie;

  // Version 3 fields (Windows Vista SP1 SEH - x86 only, unused on x64).
  ULONGLONG SEHandlerTable;
  ULONGLONG SEHandlerCount;

  // Version 4 fields (Windows 10 CFG).
  ULONGLONG GuardCFCheckFunctionPointer;
  ULONGLONG GuardCFDispatchFunctionPointer;
  ULONGLONG GuardCFFunctionTable;
  ULONGLONG GuardCFFunctionCount;
  DWORD GuardFlags;

  // Version 5 fields (Windows 10 RS1 Code Integrity).
  wincrt_code_integrity CodeIntegrity;

  // Version 6 fields (Windows 10 RS1 CFG IAT).
  ULONGLONG GuardAddressTakenIatEntryTable;
  ULONGLONG GuardAddressTakenIatEntryCount;

  // Version 7 fields (Windows 10 RS1 longjmp CFG).
  ULONGLONG GuardLongJumpTargetTable;
  ULONGLONG GuardLongJumpTargetCount;

  // Version 8 fields (Windows 10 RS2).
  ULONGLONG DynamicValueRelocTable;
  ULONGLONG CHPEMetadataPointer;

  // Version 9 fields (Windows 10 RS3 RFG - cancelled, always 0).
  ULONGLONG GuardRFFailureRoutine;
  ULONGLONG GuardRFFailureRoutineFunctionPointer;
  DWORD DynamicValueRelocTableOffset;
  WORD DynamicValueRelocTableSection;
  WORD Reserved2;
  ULONGLONG GuardRFVerifyStackPointerFunctionPointer;

  // Version 10 fields (Windows 10 RS3 Hot Patching).
  DWORD HotPatchTableOffset;
  DWORD Reserved3;

  // Version 11 fields (Windows 10 RS4 Enclave).
  ULONGLONG EnclaveConfigurationPointer;

  // Version 12 fields (Windows 10 RS5 Volatile Metadata).
  ULONGLONG VolatileMetadataPointer;

  // Version 13 fields (Windows 10 19H1 EH Continuation).
  ULONGLONG GuardEHContinuationTable;
  ULONGLONG GuardEHContinuationCount;

  // Version 14 fields (Windows 11 XFG).
  ULONGLONG GuardXFGCheckFunctionPointer;
  ULONGLONG GuardXFGDispatchFunctionPointer;
  ULONGLONG GuardXFGTableDispatchFunctionPointer;

  // Version 15 fields (Windows 11 CastGuard).
  ULONGLONG CastGuardOsDeterminedFailureMode;

  // Version 16 fields (Windows 11 GuardMemcpy).
  ULONGLONG GuardMemcpyFunctionPointer;

  // Version 17 fields (SDK 26100 - User Mode Address sanitization).
  ULONGLONG UmaFunctionPointers;
};

static_assert(sizeof(wincrt_load_config64) == 0x148, "size mismatch");

#pragma section(".rdata$T", long, read)

// clang-format off
__declspec(allocate(".rdata$T")) extern "C" const wincrt_load_config64
    _load_config_used = {
        // Size.
        sizeof(wincrt_load_config64),
        // TimeDateStamp, Version.
        0, 0, 0,
        // GlobalFlags.
        0, 0,
        // CriticalSectionDefaultTimeout.
        0,
        // DeCommit thresholds.
        0, 0,
        // LockPrefixTable.
        0,
        // MaximumAllocationSize.
        0,
        // VirtualMemoryThreshold.
        0,
        // ProcessAffinityMask.
        0,
        // ProcessHeapFlags.
        0,
        // CSDVersion, DependentLoadFlags.
        0, 0,
        // EditList.
        0,
        // SecurityCookie.
        reinterpret_cast<ULONGLONG>(&__security_cookie),
        // SEHandlerTable, SEHandlerCount (unused on x64/ARM64).
        0, 0,
        // CFG function pointers.
        reinterpret_cast<ULONGLONG>(&__guard_check_icall_fptr),
        reinterpret_cast<ULONGLONG>(&__guard_dispatch_icall_fptr),
        reinterpret_cast<ULONGLONG>(&__guard_fids_table),
        reinterpret_cast<ULONGLONG>(&__guard_fids_count),
        // GuardFlags.
        wincrt::GuardFlags::DEFAULT_CFG,
        // CodeIntegrity.
        {0, 0, 0, 0},
        // CFG IAT table.
        reinterpret_cast<ULONGLONG>(&__guard_iat_table),
        reinterpret_cast<ULONGLONG>(&__guard_iat_count),
        // CFG longjmp table.
        reinterpret_cast<ULONGLONG>(&__guard_longjmp_table),
        reinterpret_cast<ULONGLONG>(&__guard_longjmp_count),
        // DynamicValueRelocTable (linker handles).
        0,
        // CHPEMetadataPointer (ARM64EC only).
#if defined(__arm64ec__)
        reinterpret_cast<ULONGLONG>(&__chpe_metadata),
#else
        0,
#endif
        // RFG fields (cancelled, always 0).
        0, 0, 0, 0, 0, 0,
        // HotPatchTableOffset (linker handles with /FUNCTIONPADMIN).
        0, 0,
        // EnclaveConfigurationPointer.
        reinterpret_cast<ULONGLONG>(&__enclave_config),
        // VolatileMetadataPointer (Spectre mitigations).
        reinterpret_cast<ULONGLONG>(&__volatile_metadata),
        // EH continuation table (Itanium unwinding targets).
        reinterpret_cast<ULONGLONG>(&__guard_eh_cont_table),
        reinterpret_cast<ULONGLONG>(&__guard_eh_cont_count),
        // XFG function pointers.
        reinterpret_cast<ULONGLONG>(&__guard_xfg_check_icall_fptr),
        reinterpret_cast<ULONGLONG>(&__guard_xfg_dispatch_icall_fptr),
        reinterpret_cast<ULONGLONG>(&__guard_xfg_table_dispatch_icall_fptr),
        // CastGuard handler.
        reinterpret_cast<ULONGLONG>(&__castguard_check_failure_os_handled_fptr),
        // GuardMemcpy function pointer.
        reinterpret_cast<ULONGLONG>(&__guard_memcpy_fptr),
        // UmaFunctionPointers (User Mode Address sanitization, future use).
        0,
};
// clang-format on

#endif // __x86_64__ || __aarch64__

//===----------------------------------------------------------------------===//
// 32-bit Load Configuration Directory (x86, ARM)
//===----------------------------------------------------------------------===//

#if defined(__i386__) || defined(__arm__)

struct wincrt_load_config32 {
  // Version 1 fields (Windows XP).
  DWORD Size;
  DWORD TimeDateStamp;
  WORD MajorVersion;
  WORD MinorVersion;
  DWORD GlobalFlagsClear;
  DWORD GlobalFlagsSet;
  DWORD CriticalSectionDefaultTimeout;
  DWORD DeCommitFreeBlockThreshold;
  DWORD DeCommitTotalFreeThreshold;
  DWORD LockPrefixTable;
  DWORD MaximumAllocationSize;
  DWORD VirtualMemoryThreshold;
  DWORD ProcessAffinityMask;
  DWORD ProcessHeapFlags;
  WORD CSDVersion;
  WORD DependentLoadFlags;
  DWORD EditList;

  // Version 2 fields (Windows Vista /GS).
  DWORD SecurityCookie;

  // Version 3 fields (Windows Vista SP1 SafeSEH - x86 only).
  DWORD SEHandlerTable;
  DWORD SEHandlerCount;

  // Version 4 fields (Windows 10 CFG).
  DWORD GuardCFCheckFunctionPointer;
  DWORD GuardCFDispatchFunctionPointer;
  DWORD GuardCFFunctionTable;
  DWORD GuardCFFunctionCount;
  DWORD GuardFlags;

  // Version 5 fields (Windows 10 RS1 Code Integrity).
  wincrt_code_integrity CodeIntegrity;

  // Version 6 fields (Windows 10 RS1 CFG IAT).
  DWORD GuardAddressTakenIatEntryTable;
  DWORD GuardAddressTakenIatEntryCount;

  // Version 7 fields (Windows 10 RS1 longjmp CFG).
  DWORD GuardLongJumpTargetTable;
  DWORD GuardLongJumpTargetCount;

  // Version 8 fields (Windows 10 RS2).
  DWORD DynamicValueRelocTable;
  DWORD CHPEMetadataPointer;

  // Version 9 fields (Windows 10 RS3 RFG - cancelled, always 0).
  DWORD GuardRFFailureRoutine;
  DWORD GuardRFFailureRoutineFunctionPointer;
  DWORD DynamicValueRelocTableOffset;
  WORD DynamicValueRelocTableSection;
  WORD Reserved2;
  DWORD GuardRFVerifyStackPointerFunctionPointer;

  // Version 10 fields (Windows 10 RS3 Hot Patching).
  DWORD HotPatchTableOffset;
  DWORD Reserved3;

  // Version 11 fields (Windows 10 RS4 Enclave).
  DWORD EnclaveConfigurationPointer;

  // Version 12 fields (Windows 10 RS5 Volatile Metadata).
  DWORD VolatileMetadataPointer;

  // Version 13 fields (Windows 10 19H1 EH Continuation).
  DWORD GuardEHContinuationTable;
  DWORD GuardEHContinuationCount;

  // Version 14 fields (Windows 11 XFG).
  DWORD GuardXFGCheckFunctionPointer;
  DWORD GuardXFGDispatchFunctionPointer;
  DWORD GuardXFGTableDispatchFunctionPointer;

  // Version 15 fields (Windows 11 CastGuard).
  DWORD CastGuardOsDeterminedFailureMode;

  // Version 16 fields (Windows 11 GuardMemcpy).
  DWORD GuardMemcpyFunctionPointer;

  // Version 17 fields (SDK 26100 - User Mode Address sanitization).
  DWORD UmaFunctionPointers;
};

static_assert(sizeof(wincrt_load_config32) == 0xC4, "size mismatch");

#pragma section(".rdata$T", long, read)

// clang-format off
__declspec(allocate(".rdata$T")) extern "C" const wincrt_load_config32
    _load_config_used = {
        // Size.
        sizeof(wincrt_load_config32),
        // TimeDateStamp, Version.
        0, 0, 0,
        // GlobalFlags.
        0, 0,
        // CriticalSectionDefaultTimeout.
        0,
        // DeCommit thresholds.
        0, 0,
        // LockPrefixTable.
        0,
        // MaximumAllocationSize.
        0,
        // VirtualMemoryThreshold.
        0,
        // ProcessAffinityMask.
        0,
        // ProcessHeapFlags.
        0,
        // CSDVersion, DependentLoadFlags.
        0, 0,
        // EditList.
        0,
        // SecurityCookie.
        reinterpret_cast<DWORD>(&__security_cookie),
        // SEHandlerTable, SEHandlerCount (SafeSEH - x86 only).
#if defined(__i386__)
        reinterpret_cast<DWORD>(&__safe_se_handler_table),
        reinterpret_cast<DWORD>(&__safe_se_handler_count),
#else
        0, 0,
#endif
        // CFG function pointers.
        reinterpret_cast<DWORD>(&__guard_check_icall_fptr),
        // GuardCFDispatchFunctionPointer (x64/ARM64 only, 0 for x86/ARM).
        0,
        reinterpret_cast<DWORD>(&__guard_fids_table),
        reinterpret_cast<DWORD>(&__guard_fids_count),
        // GuardFlags.
        wincrt::GuardFlags::DEFAULT_CFG,
        // CodeIntegrity.
        {0, 0, 0, 0},
        // CFG IAT table.
        reinterpret_cast<DWORD>(&__guard_iat_table),
        reinterpret_cast<DWORD>(&__guard_iat_count),
        // CFG longjmp table.
        reinterpret_cast<DWORD>(&__guard_longjmp_table),
        reinterpret_cast<DWORD>(&__guard_longjmp_count),
        // DynamicValueRelocTable (linker handles).
        0,
        // CHPEMetadataPointer (not used on 32-bit).
        0,
        // RFG fields (cancelled, always 0).
        0, 0, 0, 0, 0, 0,
        // HotPatchTableOffset (linker handles with /FUNCTIONPADMIN).
        0, 0,
        // EnclaveConfigurationPointer.
        reinterpret_cast<DWORD>(&__enclave_config),
        // VolatileMetadataPointer (Spectre mitigations).
        reinterpret_cast<DWORD>(&__volatile_metadata),
        // EH continuation table (Itanium unwinding targets).
        reinterpret_cast<DWORD>(&__guard_eh_cont_table),
        reinterpret_cast<DWORD>(&__guard_eh_cont_count),
        // XFG function pointers.
        reinterpret_cast<DWORD>(&__guard_xfg_check_icall_fptr),
        reinterpret_cast<DWORD>(&__guard_xfg_dispatch_icall_fptr),
        reinterpret_cast<DWORD>(&__guard_xfg_table_dispatch_icall_fptr),
        // CastGuard handler.
        reinterpret_cast<DWORD>(&__castguard_check_failure_os_handled_fptr),
        // GuardMemcpy function pointer.
        reinterpret_cast<DWORD>(&__guard_memcpy_fptr),
        // UmaFunctionPointers (User Mode Address sanitization, future use).
        0,
};
// clang-format on

#endif // __i386__ || __arm__

//===----------------------------------------------------------------------===//
// Force inclusion of _load_config_used
//===----------------------------------------------------------------------===//

#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:__load_config_used")
#else
#pragma comment(linker, "/INCLUDE:_load_config_used")
#endif

#endif // _WIN32
