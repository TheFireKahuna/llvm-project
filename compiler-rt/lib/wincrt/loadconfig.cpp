//===-- loadconfig.cpp - Load configuration directory ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// _load_config_used tells the loader where the security cookie, the Control
// Flow Guard pointers and the guard and EH continuation tables are. The
// linker defines the tables, their counts and the guard flags as absolute
// symbols, zero when a feature is off, so the directory is constant data.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <stdint.h>

extern "C" {
extern uintptr_t __security_cookie;
extern void *volatile __guard_check_icall_fptr;
extern void *volatile __guard_dispatch_icall_fptr;

// Defined by the linker.
extern char __guard_flags[];
extern char __guard_fids_table[];
extern char __guard_fids_count[];
extern char __guard_iat_table[];
extern char __guard_iat_count[];
extern char __guard_longjmp_table[];
extern char __guard_longjmp_count[];
extern char __guard_eh_cont_table[];
extern char __guard_eh_cont_count[];
extern char __enclave_config[];
}

#define WINCRT_ADDRESS(Symbol) reinterpret_cast<ULONGLONG>(&Symbol)

#pragma section(".rdata$T", read)

// clang-format off
extern "C" __declspec(allocate(".rdata$T")) const LoadConfig
    _load_config_used = {
        sizeof(LoadConfig),
        0, 0, 0,                              // TimeDateStamp, version
        0, 0,                                 // GlobalFlags
        0,                                    // CriticalSectionDefaultTimeout
        0, 0,                                 // DeCommit thresholds
        0,                                    // LockPrefixTable
        0,                                    // MaximumAllocationSize
        0,                                    // VirtualMemoryThreshold
        0,                                    // ProcessAffinityMask
        0,                                    // ProcessHeapFlags
        0,                                    // CSDVersion
        0,                                    // DependentLoadFlags
        0,                                    // EditList
        WINCRT_ADDRESS(__security_cookie),
        0, 0,                                 // SEHandlerTable, SEHandlerCount
        WINCRT_ADDRESS(__guard_check_icall_fptr),
        WINCRT_ADDRESS(__guard_dispatch_icall_fptr),
        WINCRT_ADDRESS(__guard_fids_table),
        WINCRT_ADDRESS(__guard_fids_count),
        WINCRT_ADDRESS(__guard_flags),
        0, 0,                                 // CodeIntegrity
        WINCRT_ADDRESS(__guard_iat_table),
        WINCRT_ADDRESS(__guard_iat_count),
        WINCRT_ADDRESS(__guard_longjmp_table),
        WINCRT_ADDRESS(__guard_longjmp_count),
        0,                                    // DynamicValueRelocTable
        0,                                    // CHPEMetadataPointer
        0, 0,                                 // GuardRFFailureRoutine*
        0, 0, 0,                              // DynamicValueRelocTable*
        0,                                    // GuardRFVerifyStackPointer*
        0, 0,                                 // HotPatchTableOffset
        WINCRT_ADDRESS(__enclave_config),
        0,                                    // VolatileMetadataPointer
        WINCRT_ADDRESS(__guard_eh_cont_table),
        WINCRT_ADDRESS(__guard_eh_cont_count),
        0,                                    // GuardXFGCheckFunctionPointer
        0,                                    // GuardXFGDispatchFunctionPointer
        0,                                    // GuardXFGTableDispatch*
        0,                                    // CastGuardOsDeterminedFailureMode
        0,                                    // GuardMemcpyFunctionPointer
        0,                                    // UmaFunctionPointers
};
// clang-format on

WINCRT_INCLUDE(_load_config_used)

// Every table of .CRT is read-only, like .rdata, so it needs no section of its
// own.
#pragma comment(linker, "/merge:.CRT=.rdata")
