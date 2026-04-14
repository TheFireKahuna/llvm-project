//===-- Libc-internal NT exception and context types ---------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/nt/nt_peb.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Large Page Support
//===----------------------------------------------------------------------===//

// GetLargePageMinimum — reads KUSER_SHARED_DATA->LargePageMinimum directly.
// The real kernel32 function is a single `mov eax, [0x7FFE0244]; ret`.
inline SIZE_T GetLargePageMinimum() {
  return *reinterpret_cast<const volatile ULONG *>(
      static_cast<ULONG_PTR>(0x7FFE0244));
}

//===----------------------------------------------------------------------===//
// Time Functions
//===----------------------------------------------------------------------===//

// Well-known privilege LUID local values (high part is always 0).
inline constexpr ULONG SE_SYSTEMTIME_PRIVILEGE = 12;

// Monotonic time including suspend, in 100ns units. Single atomic load from
// KUSER_SHARED_DATA.InterruptTime (0x7FFE0008). This is what kernel32's
// QueryInterruptTime does — no ntdll export exists, just a direct read.
// Matches CLOCK_BOOTTIME semantics (Linux includes suspend).
inline void QueryInterruptTime(ULONGLONG *InterruptTime) {
  *InterruptTime =
      *reinterpret_cast<const volatile ULONGLONG *>(0x7FFE0008ULL);
}

// System time at tick granularity, in 100ns units since 1601-01-01.
// Single atomic load from KUSER_SHARED_DATA.SystemTime (0x7FFE0014).
// No QPC interpolation — ~1ms resolution vs RtlGetSystemTimePrecise's sub-us.
// Matches CLOCK_REALTIME_COARSE semantics.
inline void QueryCoarseSystemTime(ULONGLONG *SystemTime) {
  *SystemTime =
      *reinterpret_cast<const volatile ULONGLONG *>(0x7FFE0014ULL);
}

//===----------------------------------------------------------------------===//
// Context Continuation — NtContinue / NtContinueEx
//===----------------------------------------------------------------------===//

// KCONTINUE_TYPE — specifies the type of continuation for NtContinueEx.
enum KCONTINUE_TYPE : ULONG {
  KCONTINUE_UNWIND = 0,   // Unwind continuation
  KCONTINUE_RESUME = 1,   // Resume normal execution
  KCONTINUE_LONGJUMP = 2, // Longjmp-style continuation (skip destructors)
  KCONTINUE_SET = 3,      // Set context without resuming
  KCONTINUE_LAST
};

// KCONTINUE_ARGUMENT — extended parameter for NtContinueEx.
struct KCONTINUE_ARGUMENT {
  KCONTINUE_TYPE ContinueType;
  ULONG ContinueFlags;
  ULONGLONG Reserved[2];
};

// Flags for KCONTINUE_ARGUMENT.ContinueFlags.
inline constexpr ULONG KCONTINUE_FLAG_TEST_ALERT = 0x00000001;
inline constexpr ULONG KCONTINUE_FLAG_DELIVER_APC = 0x00000002;

//===----------------------------------------------------------------------===//
// XSTATE Feature Definitions
//===----------------------------------------------------------------------===//

// XSTATE feature bits (x86/x64):
//   0=x87, 1=SSE, 2=AVX, 3=BNDREGS, 4=BNDCSR(persistent),
//   5=KMASK, 6=ZMM_H, 7=ZMM, 8=IPT(supervisor), 10=PASID(supervisor),
//   11=CET_U(supervisor), 12=CET_S(supervisor, SK intercept only),
//   17=TILE_CONFIG, 18=TILE_DATA(XFD, large), 62=LWP(persistent),
//   63=RZ0(reserved)

#define XSTATE_LEGACY_FLOATING_POINT        (0)
#define XSTATE_LEGACY_SSE                   (1)
#define XSTATE_GSSE                         (2)
#define XSTATE_AVX                          (XSTATE_GSSE)
#define XSTATE_MPX_BNDREGS                  (3)
#define XSTATE_MPX_BNDCSR                   (4)
#define XSTATE_AVX512_KMASK                 (5)
#define XSTATE_AVX512_ZMM_H                 (6)
#define XSTATE_AVX512_ZMM                   (7)
#define XSTATE_IPT                          (8)
#define XSTATE_PASID                        (10)
#define XSTATE_CET_U                        (11)
#define XSTATE_CET_S                        (12)
#define XSTATE_AMX_TILE_CONFIG              (17)
#define XSTATE_AMX_TILE_DATA                (18)
#define XSTATE_LWP                          (62)
#define MAXIMUM_XSTATE_FEATURES             (64)

// XSTATE feature masks (x86/x64).
#define XSTATE_MASK_LEGACY_FLOATING_POINT   (1ULL << (XSTATE_LEGACY_FLOATING_POINT))
#define XSTATE_MASK_LEGACY_SSE              (1ULL << (XSTATE_LEGACY_SSE))

#define XSTATE_MASK_GSSE                    (1ULL << (XSTATE_GSSE))
#define XSTATE_MASK_AVX                     (XSTATE_MASK_GSSE)
#define XSTATE_MASK_MPX                     ((1ULL << (XSTATE_MPX_BNDREGS)) | \
                                             (1ULL << (XSTATE_MPX_BNDCSR)))

#define XSTATE_MASK_AVX512                  ((1ULL << (XSTATE_AVX512_KMASK)) | \
                                             (1ULL << (XSTATE_AVX512_ZMM_H)) | \
                                             (1ULL << (XSTATE_AVX512_ZMM)))

#define XSTATE_MASK_IPT                     (1ULL << (XSTATE_IPT))
#define XSTATE_MASK_PASID                   (1ULL << (XSTATE_PASID))
#define XSTATE_MASK_CET_U                   (1ULL << (XSTATE_CET_U))
#define XSTATE_MASK_CET_S                   (1ULL << (XSTATE_CET_S))
#define XSTATE_MASK_AMX_TILE_CONFIG         (1ULL << (XSTATE_AMX_TILE_CONFIG))
#define XSTATE_MASK_AMX_TILE_DATA           (1ULL << (XSTATE_AMX_TILE_DATA))
#define XSTATE_MASK_LWP                     (1ULL << (XSTATE_LWP))

// XSTATE feature bits (ARM64): 0=unused, 1=unused, 2=SVE.
#define XSTATE_ARM64_SVE                    (2)

// XSTATE feature masks (ARM64).
#define XSTATE_MASK_ARM64_SVE               (1ULL << (XSTATE_ARM64_SVE))

#if defined(__x86_64__)

#define XSTATE_MASK_LEGACY                  (XSTATE_MASK_LEGACY_FLOATING_POINT | \
                                             XSTATE_MASK_LEGACY_SSE)

#define XSTATE_MASK_ALLOWED                 (XSTATE_MASK_LEGACY | \
                                             XSTATE_MASK_AVX | \
                                             XSTATE_MASK_MPX | \
                                             XSTATE_MASK_AVX512 | \
                                             XSTATE_MASK_IPT | \
                                             XSTATE_MASK_PASID | \
                                             XSTATE_MASK_CET_U | \
                                             XSTATE_MASK_AMX_TILE_CONFIG | \
                                             XSTATE_MASK_AMX_TILE_DATA | \
                                             XSTATE_MASK_LWP)

#define XSTATE_MASK_PERSISTENT              ((1ULL << (XSTATE_MPX_BNDCSR)) | \
                                             XSTATE_MASK_LWP)

#define XSTATE_MASK_USER_VISIBLE_SUPERVISOR (XSTATE_MASK_CET_U)

#define XSTATE_MASK_LARGE_FEATURES          (XSTATE_MASK_AMX_TILE_DATA)

#define XSTATE_FIRST_NON_LEGACY_FEATURE     XSTATE_AVX

#elif defined(__i386__)

#define XSTATE_MASK_LEGACY                  (XSTATE_MASK_LEGACY_FLOATING_POINT | \
                                             XSTATE_MASK_LEGACY_SSE)

#define XSTATE_MASK_ALLOWED                 (XSTATE_MASK_LEGACY | \
                                             XSTATE_MASK_AVX | \
                                             XSTATE_MASK_MPX | \
                                             XSTATE_MASK_AVX512 | \
                                             XSTATE_MASK_IPT | \
                                             XSTATE_MASK_CET_U | \
                                             XSTATE_MASK_LWP)

#define XSTATE_MASK_PERSISTENT              ((1ULL << (XSTATE_MPX_BNDCSR)) | \
                                             XSTATE_MASK_LWP)

#define XSTATE_MASK_USER_VISIBLE_SUPERVISOR (XSTATE_MASK_CET_U)

#define XSTATE_MASK_LARGE_FEATURES          (0ULL)

#define XSTATE_FIRST_NON_LEGACY_FEATURE     XSTATE_AVX

#elif defined(__aarch64__)

#define XSTATE_MASK_LEGACY                  (0ULL)

#define XSTATE_MASK_ALLOWED                 (XSTATE_MASK_ARM64_SVE)

#define XSTATE_MASK_PERSISTENT              (0ULL)

#define XSTATE_MASK_USER_VISIBLE_SUPERVISOR (0ULL)

#define XSTATE_MASK_LARGE_FEATURES          (0ULL)

#define XSTATE_FIRST_NON_LEGACY_FEATURE     XSTATE_ARM64_SVE

#endif

#define XSTATE_MASK_AMD64_LEGACY            (XSTATE_MASK_LEGACY_FLOATING_POINT | \
                                             XSTATE_MASK_LEGACY_SSE)

// Large XSTATE features are not supported on x86.
#if defined(__i386__)
static_assert((XSTATE_MASK_ALLOWED & XSTATE_MASK_LARGE_FEATURES) == 0, "");
#endif

// Compaction mask flags.
#define XSTATE_COMPACTION_ENABLE            (63)
#define XSTATE_COMPACTION_ENABLE_MASK       (1ULL << (XSTATE_COMPACTION_ENABLE))

#define XSTATE_ALIGN_BIT                    (1)
#define XSTATE_ALIGN_MASK                   (1ULL << (XSTATE_ALIGN_BIT))

#define XSTATE_XFD_BIT                      (2)
#define XSTATE_XFD_MASK                     (1ULL << (XSTATE_XFD_BIT))

#define XSTATE_CONTROLFLAG_XSAVEOPT_MASK    (1)
#define XSTATE_CONTROLFLAG_XSAVEC_MASK      (2)
#define XSTATE_CONTROLFLAG_XFD_MASK         (4)
#define XSTATE_CONTROLFLAG_VALID_MASK       (XSTATE_CONTROLFLAG_XSAVEOPT_MASK | \
                                             XSTATE_CONTROLFLAG_XSAVEC_MASK | \
                                             XSTATE_CONTROLFLAG_XFD_MASK)

// Extended processor state configuration.
typedef struct _XSTATE_FEATURE {
    ULONG Offset;
    ULONG Size;
} XSTATE_FEATURE, *PXSTATE_FEATURE;

typedef struct _XSTATE_CONFIGURATION {
    ULONG64 EnabledFeatures;
    ULONG64 EnabledVolatileFeatures;
    ULONG Size;

    union {
        ULONG ControlFlags;
        struct {
            ULONG OptimizedSave : 1;
            ULONG CompactionEnabled : 1;
            ULONG ExtendedFeatureDisable : 1;
        };
    };

    XSTATE_FEATURE Features[MAXIMUM_XSTATE_FEATURES];
    ULONG64 EnabledSupervisorFeatures;
    // Features requiring 64-byte aligned start address.
    ULONG64 AlignedFeatures;
    ULONG AllFeatureSize;
    // Per-feature sizes for all user and supervisor states.
    ULONG AllFeatures[MAXIMUM_XSTATE_FEATURES];
    ULONG64 EnabledUserVisibleSupervisorFeatures;
    // Features that can be disabled via XFD.
    ULONG64 ExtendedFeatureDisableFeatures;
    // Save area size for non-large user and supervisor states.
    ULONG AllNonLargeFeatureSize;
    // Maximum ARM64 SVE vector length in the current environment (bytes).
    USHORT MaxSveVectorLength;
    USHORT Spare1;
} XSTATE_CONFIGURATION, *PXSTATE_CONFIGURATION;

//===----------------------------------------------------------------------===//
// KUSER_SHARED_DATA
//===----------------------------------------------------------------------===//

// Layout is identical for 32- and 64-bit (required for WoW64). New fields
// may only be appended or placed in gaps. Platform-specific fields are
// included on all architectures.

// NX support policy values.
#define NX_SUPPORT_POLICY_ALWAYSOFF     0
#define NX_SUPPORT_POLICY_ALWAYSON      1
#define NX_SUPPORT_POLICY_OPTIN         2
#define NX_SUPPORT_POLICY_OPTOUT        3

// SEH chain validation policies (values are load-bearing for ldr).
#define SEH_VALIDATION_POLICY_ON        0
#define SEH_VALIDATION_POLICY_OFF       1
#define SEH_VALIDATION_POLICY_TELEMETRY 2
#define SEH_VALIDATION_POLICY_DEFER     3

// Global shared data flags.
#define SHARED_GLOBAL_FLAGS_ERROR_PORT_V                0x0
#define SHARED_GLOBAL_FLAGS_ERROR_PORT                  \
    (1UL << SHARED_GLOBAL_FLAGS_ERROR_PORT_V)

#define SHARED_GLOBAL_FLAGS_ELEVATION_ENABLED_V         0x1
#define SHARED_GLOBAL_FLAGS_ELEVATION_ENABLED           \
    (1UL << SHARED_GLOBAL_FLAGS_ELEVATION_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_VIRT_ENABLED_V              0x2
#define SHARED_GLOBAL_FLAGS_VIRT_ENABLED                \
    (1UL << SHARED_GLOBAL_FLAGS_VIRT_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_INSTALLER_DETECT_ENABLED_V  0x3
#define SHARED_GLOBAL_FLAGS_INSTALLER_DETECT_ENABLED    \
    (1UL << SHARED_GLOBAL_FLAGS_INSTALLER_DETECT_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_LKG_ENABLED_V               0x4
#define SHARED_GLOBAL_FLAGS_LKG_ENABLED                 \
    (1UL << SHARED_GLOBAL_FLAGS_LKG_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_DYNAMIC_PROC_ENABLED_V      0x5
#define SHARED_GLOBAL_FLAGS_DYNAMIC_PROC_ENABLED        \
    (1UL << SHARED_GLOBAL_FLAGS_DYNAMIC_PROC_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_CONSOLE_BROKER_ENABLED_V    0x6
#define SHARED_GLOBAL_FLAGS_CONSOLE_BROKER_ENABLED      \
    (1UL << SHARED_GLOBAL_FLAGS_CONSOLE_BROKER_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_SECURE_BOOT_ENABLED_V       0x7
#define SHARED_GLOBAL_FLAGS_SECURE_BOOT_ENABLED         \
    (1UL << SHARED_GLOBAL_FLAGS_SECURE_BOOT_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_MULTI_SESSION_SKU_V         0x8
#define SHARED_GLOBAL_FLAGS_MULTI_SESSION_SKU           \
    (1UL << SHARED_GLOBAL_FLAGS_MULTI_SESSION_SKU_V)

#define SHARED_GLOBAL_FLAGS_MULTIUSERS_IN_SESSION_SKU_V 0x9
#define SHARED_GLOBAL_FLAGS_MULTIUSERS_IN_SESSION_SKU   \
    (1UL << SHARED_GLOBAL_FLAGS_MULTIUSERS_IN_SESSION_SKU_V)

#define SHARED_GLOBAL_FLAGS_STATE_SEPARATION_ENABLED_V 0xA
#define SHARED_GLOBAL_FLAGS_STATE_SEPARATION_ENABLED   \
    (1UL << SHARED_GLOBAL_FLAGS_STATE_SEPARATION_ENABLED_V)

#define SHARED_GLOBAL_FLAGS_ADMINAPPROVALMODE_TYPE_SPLITTOKEN_V         0xB
#define SHARED_GLOBAL_FLAGS_ADMINAPPROVALMODE_TYPE_SPLITTOKEN           \
    (1UL << SHARED_GLOBAL_FLAGS_ADMINAPPROVALMODE_TYPE_SPLITTOKEN_V)

#define SHARED_GLOBAL_FLAGS_ADMINAPPROVALMODE_TYPE_SHADOWADMIN_V        0xC
#define SHARED_GLOBAL_FLAGS_ADMINAPPROVALMODE_TYPE_SHADOWADMIN          \
    (1UL << SHARED_GLOBAL_FLAGS_ADMINAPPROVALMODE_TYPE_SHADOWADMIN_V)

#define SHARED_GLOBAL_FLAGS_SET_GLOBAL_DATA_FLAG        0x40000000

#define SHARED_GLOBAL_FLAGS_CLEAR_GLOBAL_DATA_FLAG      0x80000000

// SystemCall field values.
#define SYSTEM_CALL_SYSCALL 0
#define SYSTEM_CALL_INT_2E  1

// QPC bypass flags. None may be set unless bypass is enabled (existing code
// compares to zero to detect enablement).
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_ENABLED (0x01)
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_USE_HV_PAGE (0x02)
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_DISABLE_32BIT (0x04)
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_USE_MFENCE (0x10)
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_USE_LFENCE (0x20)
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_A73_ERRATA (0x40)
#define SHARED_GLOBAL_FLAGS_QPC_BYPASS_USE_RDTSCP (0x80)

#define PROCESSOR_FEATURE_MAX 64

typedef enum _ALTERNATIVE_ARCHITECTURE_TYPE {
    StandardDesign,
    NEC98x86,
    EndAlternatives
} ALTERNATIVE_ARCHITECTURE_TYPE;

typedef struct _KUSER_SHARED_DATA {

    // Low 32-bit tick count (deprecated) and tick count multiplier.
    ULONG TickCountLowDeprecated;
    ULONG TickCountMultiplier;

    // 64-bit interrupt time in 100ns units.
    volatile KSYSTEM_TIME InterruptTime;

    // 64-bit system time in 100ns units.
    volatile KSYSTEM_TIME SystemTime;

    // 64-bit time zone bias.
    volatile KSYSTEM_TIME TimeZoneBias;

    // Image magic number range for the host system (inclusive).
    USHORT ImageNumberLow;
    USHORT ImageNumberHigh;

    // System root (use RtlGetNtSystemRoot for accuracy).
    WCHAR NtSystemRoot[260];

    ULONG MaxStackTraceDepth;
    ULONG CryptoExponent;
    ULONG TimeZoneId;
    ULONG LargePageMinimum;

    // AIT sampling rate.
    ULONG AitSamplingValue;

    // Switchback processing control.
    ULONG AppCompatFlag;

    // Kernel root RNG state seed version.
    ULONGLONG RNGSeedVersion;

    // Assertion failure handling control.
    ULONG GlobalValidationRunlevel;

    volatile LONG TimeZoneBiasStamp;

    // Build number (undecorated). GetVersionEx may hide the real value.
    ULONG NtBuildNumber;

    // Product type (use RtlGetNtProductType for accuracy).
    NT_PRODUCT_TYPE NtProductType;
    BOOLEAN ProductTypeIsValid;
    BOOLEAN Reserved0[1];
    USHORT NativeProcessorArchitecture;

    // NT version. Processes may see an altered version from their PEB;
    // these fields hold the true version.
    ULONG NtMajorVersion;
    ULONG NtMinorVersion;

    BOOLEAN ProcessorFeatures[PROCESSOR_FEATURE_MAX];

    ULONG Reserved1;
    ULONG Reserved3;

    // Time slippage accumulated while in debugger.
    volatile ULONG TimeSlip;

    ALTERNATIVE_ARCHITECTURE_TYPE AlternativeArchitecture;

    // Boot sequence number, incremented by OS loader.
    ULONG BootId;

    // Evaluation expiration (UTC, 100ns units). 0 = no expiration.
    LARGE_INTEGER SystemExpirationDate;

    // Suite mask (use RtlGetSuiteMask for accuracy).
    ULONG SuiteMask;

    BOOLEAN KdDebuggerEnabled;

    // Mitigation policies.
    union {
        UCHAR MitigationPolicies;
        struct {
            UCHAR NXSupportPolicy : 2;
            UCHAR SEHValidationPolicy : 2;
            UCHAR CurDirDevicesSkippedForDlls : 2;
            UCHAR Reserved : 2;
        };
    };

    // Single processor yield duration in cycles (for spin-wait tuning).
    USHORT CyclesPerYield;

    // Console session ID (use RtlGetActiveConsoleId for accuracy).
    volatile ULONG ActiveConsoleId;

    // Dismount serial number for handle invalidation tracking.
    volatile ULONG DismountCount;

    // 64-bit COM+ package status (IL image runtime selection).
    ULONG ComPlusPackage;

    // System-wide last user input tick count (updated ~once/minute/session).
    ULONG LastSystemRITEventTickCount;

    // Physical page count (truncated; use FullNumberOfPhysicalPages).
    ULONG NumberOfPhysicalPages;

    BOOLEAN SafeBootMode;

    union {
        UCHAR VirtualizationFlags;

#if defined(__aarch64__)

        // Keep in sync with arc.w.
        struct {
            UCHAR ArchStartedInEl2 : 1;
            UCHAR QcSlIsSupported : 1;
            UCHAR : 6;
        };

#endif

    };

    UCHAR Reserved12[2];

    // System state flags (use interlocked operations to manipulate).
    // DbgMultiSessionSku: use RtlIsMultiSessionSku for accuracy.
    union {
        ULONG SharedDataFlags;
        struct {
            // Debugger-only bit fields; use SHARED_GLOBAL_FLAGS_* instead.
            ULONG DbgErrorPortPresent       : 1;
            ULONG DbgElevationEnabled       : 1;
            ULONG DbgVirtEnabled            : 1;
            ULONG DbgInstallerDetectEnabled : 1;
            ULONG DbgLkgEnabled             : 1;
            ULONG DbgDynProcessorEnabled    : 1;
            ULONG DbgConsoleBrokerEnabled   : 1;
            ULONG DbgSecureBootEnabled      : 1;
            ULONG DbgMultiSessionSku        : 1;
            ULONG DbgMultiUsersInSessionSku : 1;
            ULONG DbgStateSeparationEnabled : 1;
            ULONG SpareBits                 : 21;
        };
    };

    ULONG DataFlagsPad[1];

    // Fast system call stub pointer (32-bit systems only).
    ULONGLONG TestRetInstruction;
    LONGLONG QpcFrequency;

    // Nonzero on AMD64 if altered system service call mechanism.
    ULONG SystemCall;

    ULONG Reserved2;

    // Full 64-bit physical page count (dynamic).
    ULONGLONG FullNumberOfPhysicalPages;

    ULONGLONG SystemCallPad[1];

    // 64-bit tick count.
    union {
        volatile KSYSTEM_TIME TickCount;
        volatile ULONG64 TickCountQuad;
        struct {
            ULONG ReservedTickCountOverlay[3];
            ULONG TickCountPad[1];
        };
    };

    // System-wide pointer encoding cookie.
    ULONG Cookie;
    ULONG CookiePad[1];

    // Foreground process in active console session
    // (use RtlGetConsoleSessionForegroundProcessId for accuracy).
    LONGLONG ConsoleSessionForegroundProcessId;

    // Precise time service data (64-byte cache-line aligned, access-ordered).
    ULONGLONG TimeUpdateLock;

    // QPC value establishing current system time.
    ULONGLONG BaselineSystemTimeQpc;

    // QPC value for last interrupt time computation.
    ULONGLONG BaselineInterruptTimeQpc;

    // Scaled system time seconds per perf count (may vary for time sync).
    ULONGLONG QpcSystemTimeIncrement;

    // Scaled interrupt time seconds per perf count (constant after boot).
    ULONGLONG QpcInterruptTimeIncrement;

    // Shift counts for perf counter to time conversion.
    UCHAR QpcSystemTimeIncrementShift;
    UCHAR QpcInterruptTimeIncrementShift;

    USHORT UnparkedProcessorCount;

    // Enclave feature bitmask (use RtlIsEnclaveFeaturePresent for accuracy).
    ULONG EnclaveFeatureMask[4];

    // Telemetry coverage round.
    ULONG TelemetryCoverageRound;

    // ETW user mode global logging (UMGL).
    USHORT UserModeGlobalLogger[16];

    // IFEO from HKCU in addition to HKLM.
    ULONG ImageFileExecutionOptions;

    // System language info generation count.
    ULONG LangGenerationCount;

    ULONGLONG Reserved4;

    // 64-bit interrupt time bias in 100ns units.
    volatile ULONGLONG InterruptTimeBias;

    // 64-bit QPC bias (pre-shift).
    volatile ULONGLONG QpcBias;

    ULONG ActiveProcessorCount;
    volatile UCHAR ActiveGroupCount;

    UCHAR Reserved9;

    union {
        USHORT QpcData;
        struct {
            // Whether QPC can bypass syscall and read counter directly.
            volatile UCHAR QpcBypassEnabled;
            UCHAR QpcReserved;
        };
    };

    LARGE_INTEGER TimeZoneBiasEffectiveStart;
    LARGE_INTEGER TimeZoneBiasEffectiveEnd;

    // Extended processor state configuration (x86/x64).
    XSTATE_CONFIGURATION XState;

    KSYSTEM_TIME FeatureConfigurationChangeStamp;
    ULONG Spare;

    ULONG64 UserPointerAuthMask;

    // Extended processor state configuration (ARM64). Reserved space on
    // other architectures is not available for reuse.
#if defined(__aarch64__)
    XSTATE_CONFIGURATION XStateArm64;
#else
    ULONG Reserved10[210];
#endif

} KUSER_SHARED_DATA, *PKUSER_SHARED_DATA;

// Layout validation. Syscall constants are load-bearing for assembler.
static_assert(SYSTEM_CALL_SYSCALL == 0, "");
static_assert(SYSTEM_CALL_INT_2E == 1, "");

// Field offset checks (must match across all architectures).
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TickCountLowDeprecated) == 0x0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TickCountMultiplier) == 0x4, "");
static_assert(__alignof(KSYSTEM_TIME) == 4, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, InterruptTime) == 0x08, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SystemTime) == 0x014, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeZoneBias) == 0x020, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ImageNumberLow) == 0x02c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ImageNumberHigh) == 0x02e, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NtSystemRoot) == 0x030, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, MaxStackTraceDepth) == 0x238, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, CryptoExponent) == 0x23c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeZoneId) == 0x240, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, LargePageMinimum) == 0x244, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, AitSamplingValue) == 0x248, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, AppCompatFlag) == 0x24c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, RNGSeedVersion) == 0x250, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, GlobalValidationRunlevel) == 0x258, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeZoneBiasStamp) == 0x25c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NtBuildNumber) == 0x260, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NtProductType) == 0x264, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ProductTypeIsValid) == 0x268, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NativeProcessorArchitecture) == 0x26a, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NtMajorVersion) == 0x26c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NtMinorVersion) == 0x270, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ProcessorFeatures) == 0x274, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved1) == 0x2b4, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved3) == 0x2b8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeSlip) == 0x2bc, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, AlternativeArchitecture) == 0x2c0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SystemExpirationDate) == 0x2c8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SuiteMask) == 0x2d0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, KdDebuggerEnabled) == 0x2d4, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, MitigationPolicies) == 0x2d5, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, CyclesPerYield) == 0x2d6, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ActiveConsoleId) == 0x2d8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, DismountCount) == 0x2dc, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ComPlusPackage) == 0x2e0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, LastSystemRITEventTickCount) == 0x2e4, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, NumberOfPhysicalPages) == 0x2e8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SafeBootMode) == 0x2ec, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, VirtualizationFlags) == 0x2ed, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved12) == 0x2ee, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SharedDataFlags) == 0x2f0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TestRetInstruction) == 0x2f8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcFrequency) == 0x300, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SystemCall) == 0x308, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved2) == 0x30c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, SystemCallPad) == 0x318, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TickCount) == 0x320, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TickCountQuad) == 0x320, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Cookie) == 0x330, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ConsoleSessionForegroundProcessId) == 0x338, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeUpdateLock) == 0x340, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, BaselineSystemTimeQpc) == 0x348, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, BaselineInterruptTimeQpc) == 0x350, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcSystemTimeIncrement) == 0x358, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcInterruptTimeIncrement) == 0x360, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcSystemTimeIncrementShift) == 0x368, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcInterruptTimeIncrementShift) == 0x369, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, UnparkedProcessorCount) == 0x36a, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, EnclaveFeatureMask) == 0x36c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TelemetryCoverageRound) == 0x37c, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, UserModeGlobalLogger) == 0x380, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ImageFileExecutionOptions) == 0x3a0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, LangGenerationCount) == 0x3a4, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved4) == 0x3a8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, InterruptTimeBias) == 0x3b0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcBias) == 0x3b8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ActiveProcessorCount) == 0x3c0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, ActiveGroupCount) == 0x3c4, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved9) == 0x3c5, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcData) == 0x3c6, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcBypassEnabled) == 0x3c6, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, QpcReserved) == 0x3c7, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeZoneBiasEffectiveStart) == 0x3c8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, TimeZoneBiasEffectiveEnd) == 0x3d0, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, XState) == 0x3d8, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, FeatureConfigurationChangeStamp) == 0x720, "");
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, UserPointerAuthMask) == 0x730, "");
#if defined(__aarch64__)
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, XStateArm64) == 0x738, "");
#else
static_assert(FIELD_OFFSET(KUSER_SHARED_DATA, Reserved10) == 0x738, "");
#endif
static_assert(sizeof(KUSER_SHARED_DATA) == 0xA80, "");

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_TYPES_H
