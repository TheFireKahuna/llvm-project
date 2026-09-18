//===-- loadconfig.cpp - PE load configuration directory ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// _load_config_used tells the loader where the security cookie, the Control
// Flow Guard tables and pointers, and the other mitigation metadata live. The
// linker defines the guard tables, counts and flags as absolute symbols, with
// zero values when a feature is absent, so the directory needs no runtime
// initialization and no fallbacks for them.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern uintptr_t __security_cookie;

extern "C" {

// cfguard.cpp
extern void *volatile __guard_check_icall_fptr;
extern void *volatile __guard_dispatch_icall_fptr;
extern void *volatile __guard_xfg_check_icall_fptr;
extern void *volatile __guard_xfg_dispatch_icall_fptr;
extern void *volatile __guard_xfg_table_dispatch_icall_fptr;
extern void *volatile __castguard_check_failure_os_handled_fptr;

// Linker-defined absolute symbols.
extern char __guard_flags[];
extern char __guard_fids_table[];
extern char __guard_fids_count[];
extern char __guard_iat_table[];
extern char __guard_iat_count[];
extern char __guard_longjmp_table[];
extern char __guard_longjmp_count[];
extern char __guard_eh_cont_table[];
extern char __guard_eh_cont_count[];

// Provided by the linker only when the corresponding feature is enabled.
extern void *__guard_memcpy_fptr;
extern char __enclave_config[];
extern char __volatile_metadata[];

__declspec(selectany) void *__wincrt_no_memcpy_fptr = nullptr;
__declspec(selectany) char __wincrt_no_enclave_config[1] = {0};
__declspec(selectany) char __wincrt_no_volatile_metadata[1] = {0};

} // extern "C"

WINCRT_ALTERNATENAME(__guard_memcpy_fptr, __wincrt_no_memcpy_fptr)
WINCRT_ALTERNATENAME(__enclave_config, __wincrt_no_enclave_config)
WINCRT_ALTERNATENAME(__volatile_metadata, __wincrt_no_volatile_metadata)

#pragma section(".rdata$T", long, read)

// IMAGE_LOAD_CONFIG_DIRECTORY64 with GuardFlags widened to a pointer. The
// flags are a linker absolute symbol; storing its address in a DWORD is not a
// constant expression in C++, but a pointer-width relocation is. The widened
// field covers GuardFlags plus the two CodeIntegrity WORDs, which are zero.
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

#define WINCRT_SAME_OFFSET(Field, SdkField)                                    \
  static_assert(offsetof(LoadConfig, Field) ==                                 \
                    offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SdkField),         \
                "layout")
static_assert(sizeof(LoadConfig) == sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),
              "layout");
WINCRT_SAME_OFFSET(SecurityCookie, SecurityCookie);
WINCRT_SAME_OFFSET(GuardCFCheckFunctionPointer, GuardCFCheckFunctionPointer);
WINCRT_SAME_OFFSET(GuardFlagsAndCodeIntegrity, GuardFlags);
WINCRT_SAME_OFFSET(CodeIntegrityCatalogOffset, CodeIntegrity.CatalogOffset);
WINCRT_SAME_OFFSET(GuardAddressTakenIatEntryTable,
                   GuardAddressTakenIatEntryTable);
WINCRT_SAME_OFFSET(GuardEHContinuationTable, GuardEHContinuationTable);
WINCRT_SAME_OFFSET(GuardXFGCheckFunctionPointer, GuardXFGCheckFunctionPointer);
WINCRT_SAME_OFFSET(CastGuardOsDeterminedFailureMode,
                   CastGuardOsDeterminedFailureMode);
WINCRT_SAME_OFFSET(GuardMemcpyFunctionPointer, GuardMemcpyFunctionPointer);
#undef WINCRT_SAME_OFFSET

#define WINCRT_RVA(Symbol) reinterpret_cast<ULONGLONG>(Symbol)

// clang-format off
__declspec(allocate(".rdata$T")) extern "C" const LoadConfig
    _load_config_used = {
        sizeof(LoadConfig),
        0, 0, 0,                                    // TimeDateStamp, version
        0, 0,                                       // GlobalFlags
        0,                                          // CriticalSectionDefaultTimeout
        0, 0,                                       // DeCommit thresholds
        0,                                          // LockPrefixTable
        0,                                          // MaximumAllocationSize
        0,                                          // VirtualMemoryThreshold
        0,                                          // ProcessAffinityMask
        0,                                          // ProcessHeapFlags
        0, 0,                                       // CSDVersion, DependentLoadFlags
        0,                                          // EditList
        WINCRT_RVA(&__security_cookie),
        0, 0,                                       // SEHandlerTable, SEHandlerCount
        WINCRT_RVA(&__guard_check_icall_fptr),
        WINCRT_RVA(&__guard_dispatch_icall_fptr),
        WINCRT_RVA(&__guard_fids_table),
        WINCRT_RVA(&__guard_fids_count),
        WINCRT_RVA(__guard_flags),                  // GuardFlags, CodeIntegrity.Flags/Catalog
        0, 0,                                       // CodeIntegrity.CatalogOffset/Reserved
        WINCRT_RVA(&__guard_iat_table),
        WINCRT_RVA(&__guard_iat_count),
        WINCRT_RVA(&__guard_longjmp_table),
        WINCRT_RVA(&__guard_longjmp_count),
        0,                                          // DynamicValueRelocTable
        0,                                          // CHPEMetadataPointer
        0, 0, 0, 0, 0, 0,                           // Return Flow Guard
        0, 0,                                       // HotPatchTableOffset, Reserved3
        WINCRT_RVA(&__enclave_config),
        WINCRT_RVA(&__volatile_metadata),
        WINCRT_RVA(&__guard_eh_cont_table),
        WINCRT_RVA(&__guard_eh_cont_count),
        WINCRT_RVA(&__guard_xfg_check_icall_fptr),
        WINCRT_RVA(&__guard_xfg_dispatch_icall_fptr),
        WINCRT_RVA(&__guard_xfg_table_dispatch_icall_fptr),
        WINCRT_RVA(&__castguard_check_failure_os_handled_fptr),
        WINCRT_RVA(&__guard_memcpy_fptr),
        0,                                          // UmaFunctionPointers
};
// clang-format on

WINCRT_INCLUDE(_load_config_used)
