//===-- NT ABI types for Windows platform runtimes -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Windows NT platform ABI types — CONTEXT, EXCEPTION_RECORD,
// DISPATCHER_CONTEXT, and related SEH structures.
//
// This is the Windows equivalent of <sys/ucontext.h> on Linux: platform
// register context and exception types published by libc for use by other
// runtimes (libunwind, compiler-rt, libc++abi).
//
// Only types that describe the OS ABI belong here. libc-internal NT APIs
// (NtContinue, NtRaiseException, VEH registration, I/O structures, etc.)
// stay in libc/src/__support/OSUtil/windows/.
//
//===----------------------------------------------------------------------===//

#ifndef _SYS_NTABI_H
#define _SYS_NTABI_H

#include "../__llvm-libc-common.h"

#ifdef __cplusplus
static_assert(sizeof(void *) == 8, "sys/ntabi.h targets 64-bit Windows only");
#else
_Static_assert(sizeof(void *) == 8, "sys/ntabi.h targets 64-bit Windows only");
#endif

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Calling Conventions
//===----------------------------------------------------------------------===//

// NTAPI / WINAPI — NT kernel and Win32 calling conventions.
//
// On i386, both are __stdcall (callee cleans up the stack). On x86-64 they
// delegate to LIBC_MSABI, which resolves to [[gnu::ms_abi]] under C++ /
// C23 and __attribute__((ms_abi)) in older C — forces MS x64 ABI even when
// the translation unit's default is not MS x64, essential for ntdll /
// bcryptprimitives / sspicli imports and for kernel/loader callbacks. On
// ARM64, Windows uses AAPCS64 which matches the platform default, so the
// macro is empty.
//
// The [[gnu::ms_abi]] form enforces standard attribute placement: NTAPI
// must appear at the start of a declaration (before the return type), and
// cannot be placed on a function *type* at all (GCC rejects it; Clang
// warns under -Wgcc-compat). For function-pointer typedefs in C++, use
// `decltype` on an unreferenced external prototype (see
// PVECTORED_EXCEPTION_HANDLER below).
#ifndef NTAPI
#  if defined(__i386__) || defined(_M_IX86)
#    define NTAPI __attribute__((stdcall))
#  else
#    define NTAPI LIBC_MSABI
#  endif
#endif
#ifndef WINAPI
#  if defined(__i386__) || defined(_M_IX86)
#    define WINAPI __attribute__((stdcall))
#  else
#    define WINAPI LIBC_MSABI
#  endif
#endif

//===----------------------------------------------------------------------===//
// Scalar Types
//===----------------------------------------------------------------------===//

typedef unsigned char BOOLEAN;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef char CHAR;
typedef unsigned short USHORT;
typedef unsigned short WORD;
typedef short SHORT;
typedef unsigned int UINT32;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned long DWORD;
typedef unsigned long long DWORD64;
typedef unsigned long long ULONGLONG;
typedef ULONGLONG *PULONGLONG;
typedef long long LONGLONG;
typedef long long LONG64;
typedef LONGLONG *PLONGLONG;
typedef unsigned long long ULONG64;
typedef unsigned long long ULONG_PTR;
typedef ULONG_PTR DWORD_PTR;
typedef long long LONG_PTR;
typedef unsigned long long SIZE_T;
typedef int BOOL;
typedef long NTSTATUS;
typedef long HRESULT;
#ifndef S_OK
#ifdef __cplusplus
inline constexpr HRESULT S_OK = 0;
#else
#define S_OK ((HRESULT)0L)
#endif
#endif
#ifndef S_FALSE
#ifdef __cplusplus
inline constexpr HRESULT S_FALSE = 1;
#else
#define S_FALSE ((HRESULT)1L)
#endif
#endif
typedef DWORD LCID;
typedef ULONG LOGICAL;

// Pointer types
typedef void *PVOID;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef void *HANDLE;

// Legacy thread affinity.
typedef ULONG_PTR KAFFINITY;
typedef KAFFINITY *PKAFFINITY;

// Variable-length trailing array marker — identical to the SDK's ANYSIZE_ARRAY.
// Used in structures where the final field is a variable-length array sized by
// an earlier count field (e.g., GroupCount). The [1] makes the struct a valid
// union member while allowing indexed access beyond the declared bound.
#define ANYSIZE_ARRAY 1

// Group-relative logical processor identifier. Used by
// NtGetCurrentProcessorNumberEx, ThreadIdealProcessorEx, and TEB fields.
typedef struct _PROCESSOR_NUMBER {
  USHORT Group;
  UCHAR Number;
  UCHAR Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;

// Wide-char type — 16-bit even when wchar_t is 32-bit (llvm-libc + POSIX mode).
#ifdef __cplusplus
using WCHAR = char16_t;
using LPCWSTR = const char16_t *;
using LPWSTR = char16_t *;
#else
typedef unsigned short WCHAR;
typedef const WCHAR *LPCWSTR;
typedef WCHAR *LPWSTR;
#endif

//===----------------------------------------------------------------------===//
// NT ABI Structures — process, thread, and system query types
//===----------------------------------------------------------------------===//

// UNICODE_STRING — counted wide string used by all Nt* APIs.
struct UNICODE_STRING {
  USHORT Length;        // Size in bytes (not characters), excluding NUL.
  USHORT MaximumLength; // Buffer capacity in bytes.
  WCHAR *Buffer;
};
#ifdef __cplusplus
using PUNICODE_STRING = UNICODE_STRING *;
using PCUNICODE_STRING = const UNICODE_STRING *;
#else
typedef struct UNICODE_STRING *PUNICODE_STRING;
typedef const struct UNICODE_STRING *PCUNICODE_STRING;
#endif

// Process virtual memory counters — returned by
// NtQueryInformationProcess(ProcessVmCounters).
struct VM_COUNTERS {
  SIZE_T PeakVirtualSize;          // Peak virtual address space
  SIZE_T VirtualSize;              // Current virtual address space
  ULONG PageFaultCount;
  SIZE_T PeakWorkingSetSize;       // Peak pages resident in RAM
  SIZE_T WorkingSetSize;           // Current pages resident in RAM
  SIZE_T QuotaPeakPagedPoolUsage;
  SIZE_T QuotaPagedPoolUsage;
  SIZE_T QuotaPeakNonPagedPoolUsage;
  SIZE_T QuotaNonPagedPoolUsage;
  SIZE_T PagefileUsage;            // Current commit charge (private)
  SIZE_T PeakPagefileUsage;        // Peak commit charge
};

// Processor group affinity mask.
typedef struct _GROUP_AFFINITY {
  KAFFINITY Mask;
  WORD Group;
  WORD Reserved[3];
} GROUP_AFFINITY, *PGROUP_AFFINITY;

// Processor topology — from
// NtQuerySystemInformation(SystemLogicalProcessorInformationEx).

#define LTP_PC_SMT 0x1 // Core has symmetric multithreading (hyperthreading).

typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP {
  RelationProcessorCore,
  RelationNumaNode,
  RelationCache,
  RelationProcessorPackage,
  RelationGroup,
  RelationProcessorDie,
  RelationNumaNodeEx,
  RelationProcessorModule,
  RelationAll = 0xffff
} LOGICAL_PROCESSOR_RELATIONSHIP;

typedef enum _PROCESSOR_CACHE_TYPE {
  CacheUnified,
  CacheInstruction,
  CacheData,
  CacheTrace,
  CacheUnknown
} PROCESSOR_CACHE_TYPE, *PPROCESSOR_CACHE_TYPE;

typedef struct _CACHE_DESCRIPTOR {
  BYTE Level;
  BYTE Associativity;
  WORD LineSize;
  DWORD Size;
  PROCESSOR_CACHE_TYPE Type;
} CACHE_DESCRIPTOR, *PCACHE_DESCRIPTOR;

typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION {
  ULONG_PTR ProcessorMask;
  LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
  union {
    struct {
      UCHAR Flags;
    } ProcessorCore;
    struct {
      ULONG NodeNumber;
    } NumaNode;
    CACHE_DESCRIPTOR Cache;
    ULONGLONG Reserved[2];
  };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION;

typedef struct _PROCESSOR_RELATIONSHIP {
  UCHAR Flags;
  UCHAR EfficiencyClass;
  UCHAR Reserved[20];
  USHORT GroupCount;
  GROUP_AFFINITY GroupMask[ANYSIZE_ARRAY];
} PROCESSOR_RELATIONSHIP, *PPROCESSOR_RELATIONSHIP;

typedef struct _NUMA_NODE_RELATIONSHIP {
  ULONG NodeNumber;
  UCHAR Reserved[18];
  USHORT GroupCount;
  union {
    GROUP_AFFINITY GroupMask;
    GROUP_AFFINITY GroupMasks[ANYSIZE_ARRAY];
  };
} NUMA_NODE_RELATIONSHIP, *PNUMA_NODE_RELATIONSHIP;

typedef struct _CACHE_RELATIONSHIP {
  UCHAR Level;
  UCHAR Associativity;
  USHORT LineSize;
  ULONG CacheSize;
  PROCESSOR_CACHE_TYPE Type;
  UCHAR Reserved[18];
  USHORT GroupCount;
  union {
    GROUP_AFFINITY GroupMask;
    GROUP_AFFINITY GroupMasks[ANYSIZE_ARRAY];
  };
} CACHE_RELATIONSHIP, *PCACHE_RELATIONSHIP;

typedef struct _PROCESSOR_GROUP_INFO {
  UCHAR MaximumProcessorCount;
  UCHAR ActiveProcessorCount;
  UCHAR Reserved[38];
  KAFFINITY ActiveProcessorMask;
} PROCESSOR_GROUP_INFO, *PPROCESSOR_GROUP_INFO;

typedef struct _GROUP_RELATIONSHIP {
  USHORT MaximumGroupCount;
  USHORT ActiveGroupCount;
  UCHAR Reserved[20];
  PROCESSOR_GROUP_INFO GroupInfo[ANYSIZE_ARRAY];
} GROUP_RELATIONSHIP, *PGROUP_RELATIONSHIP;

// Variable-sized container returned by
// NtQuerySystemInformation(SystemLogicalProcessorInformationEx).
// Walk the packed array by advancing the pointer by each entry's Size field.
typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
  enum _LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
  DWORD Size;
  union {
    struct _PROCESSOR_RELATIONSHIP Processor;
    struct _NUMA_NODE_RELATIONSHIP NumaNode;
    struct _CACHE_RELATIONSHIP Cache;
    struct _GROUP_RELATIONSHIP Group;
  };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

#ifdef __cplusplus
// ABI layout validation — catch struct packing mismatches at compile time.
static_assert(sizeof(GROUP_AFFINITY) == 16,
              "GROUP_AFFINITY must be 16 bytes");
static_assert(sizeof(PROCESSOR_NUMBER) == 4,
              "PROCESSOR_NUMBER must be 4 bytes");
static_assert(sizeof(PROCESSOR_GROUP_INFO) == 48,
              "PROCESSOR_GROUP_INFO must be 48 bytes");
static_assert(sizeof(CACHE_DESCRIPTOR) == 12,
              "CACHE_DESCRIPTOR must be 12 bytes");
// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX offset checks:
// Relationship at 0, Size at 4, union at 8.
static_assert(
    __builtin_offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Size) == 4,
    "SLPI_EX::Size must be at offset 4");
static_assert(
    __builtin_offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) == 8,
    "SLPI_EX union must start at offset 8");
// SYSTEM_LOGICAL_PROCESSOR_INFORMATION: 8-byte mask, 4-byte enum, 4-byte pad,
// 16-byte union (sized by ULONGLONG Reserved[2]).
static_assert(sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) == 32,
              "SYSTEM_LOGICAL_PROCESSOR_INFORMATION must be 32 bytes");
// UNICODE_STRING: two USHORTs followed by a pointer-width Buffer.
static_assert(sizeof(UNICODE_STRING) == 16,
              "UNICODE_STRING must be 16 bytes");
static_assert(__builtin_offsetof(UNICODE_STRING, Buffer) == 8,
              "UNICODE_STRING::Buffer must be at offset 8");
// VM_COUNTERS: ULONG PageFaultCount introduces 4-byte pad before SIZE_T fields.
static_assert(sizeof(VM_COUNTERS) == 88,
              "VM_COUNTERS must be 88 bytes");
static_assert(__builtin_offsetof(VM_COUNTERS, PeakWorkingSetSize) == 24,
              "VM_COUNTERS::PeakWorkingSetSize must be at offset 24");
#endif

//===----------------------------------------------------------------------===//
// Exception Codes (NTSTATUS values raised by hardware or RtlRaiseException)
//===----------------------------------------------------------------------===//

#define EXCEPTION_ACCESS_VIOLATION         0xC0000005
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED    0xC000008C
#define EXCEPTION_BREAKPOINT               0x80000003
#define EXCEPTION_DATATYPE_MISALIGNMENT    0x80000002
#define EXCEPTION_FLT_DENORMAL_OPERAND     0xC000008D
#define EXCEPTION_FLT_DIVIDE_BY_ZERO       0xC000008E
#define EXCEPTION_FLT_INEXACT_RESULT       0xC000008F
#define EXCEPTION_FLT_INVALID_OPERATION    0xC0000090
#define EXCEPTION_FLT_OVERFLOW             0xC0000091
#define EXCEPTION_FLT_STACK_CHECK          0xC0000092
#define EXCEPTION_FLT_UNDERFLOW            0xC0000093
#define EXCEPTION_GUARD_PAGE               0x80000001
#define EXCEPTION_ILLEGAL_INSTRUCTION      0xC000001D
#define EXCEPTION_IN_PAGE_ERROR            0xC0000006
#define EXCEPTION_INT_DIVIDE_BY_ZERO       0xC0000094
#define EXCEPTION_INT_OVERFLOW             0xC0000095
#define EXCEPTION_PRIV_INSTRUCTION         0xC0000096
#define EXCEPTION_SINGLE_STEP              0x80000004
#define EXCEPTION_STACK_OVERFLOW           0xC00000FD

// Debug exception codes (informational, severity 01)
#define DBG_PRINTEXCEPTION_C               0x40010006
#define DBG_PRINTEXCEPTION_WIDE_C          0x4001000A

//===----------------------------------------------------------------------===//
// VEH / SEH Filter Return Values
//===----------------------------------------------------------------------===//

// Return values for VEH handlers and SEH __except filter expressions.
#define EXCEPTION_CONTINUE_EXECUTION  (-1)
#define EXCEPTION_CONTINUE_SEARCH      0
#define EXCEPTION_EXECUTE_HANDLER      1

//===----------------------------------------------------------------------===//
// SEH Filter Intrinsics
//===----------------------------------------------------------------------===//

#define GetExceptionCode()        __exception_code()
#define GetExceptionInformation() __exception_info()

//===----------------------------------------------------------------------===//
// Exception Handling Constants
//===----------------------------------------------------------------------===//

#define EXCEPTION_MAXIMUM_PARAMETERS 15

// ExceptionFlags bits — distinguish search phase from unwind phase.
#define EXCEPTION_NONCONTINUABLE   0x01
#define EXCEPTION_UNWINDING        0x02
#define EXCEPTION_EXIT_UNWIND      0x04
#define EXCEPTION_TARGET_UNWIND    0x20
#define EXCEPTION_COLLIDED_UNWIND  0x40
#define EXCEPTION_UNWIND           (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | \
                                    EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND)

#define IS_UNWINDING(Flag)       ((Flag) & EXCEPTION_UNWINDING)
#define IS_TARGET_UNWIND(Flag)   ((Flag) & EXCEPTION_TARGET_UNWIND)
#define IS_DISPATCHING(Flag)     (((Flag) & EXCEPTION_UNWIND) == 0)

// NTSTATUS severity field (bits 30-31).
#define STATUS_SEVERITY_SUCCESS  0x0

//===----------------------------------------------------------------------===//
// EXCEPTION_DISPOSITION — SEH handler return values
//===----------------------------------------------------------------------===//

#ifndef _EXCEPTION_DISPOSITION_DEFINED
#define _EXCEPTION_DISPOSITION_DEFINED
typedef enum _EXCEPTION_DISPOSITION {
  ExceptionContinueExecution = 0,
  ExceptionContinueSearch = 1,
  ExceptionNestedException = 2,
  ExceptionCollidedUnwind = 3
} EXCEPTION_DISPOSITION;
#endif

//===----------------------------------------------------------------------===//
// EXCEPTION_RECORD
//===----------------------------------------------------------------------===//

typedef struct _EXCEPTION_RECORD {
  DWORD ExceptionCode;
  DWORD ExceptionFlags;
  struct _EXCEPTION_RECORD *ExceptionRecord;
  PVOID ExceptionAddress;
  DWORD NumberParameters;
  ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;

typedef EXCEPTION_RECORD *PEXCEPTION_RECORD;

//===----------------------------------------------------------------------===//
// Architecture-Specific CONTEXT
//===----------------------------------------------------------------------===//

#if defined(__x86_64__) || defined(_M_X64)

typedef struct __attribute__((aligned(16))) _M128A {
  ULONGLONG Low;
  LONGLONG High;
} M128A;

typedef M128A *PM128A;

typedef struct __attribute__((aligned(16))) _XSAVE_FORMAT {
  WORD ControlWord;
  WORD StatusWord;
  BYTE TagWord;
  BYTE Reserved1;
  WORD ErrorOpcode;
  DWORD ErrorOffset;
  WORD ErrorSelector;
  WORD Reserved2;
  DWORD DataOffset;
  WORD DataSelector;
  WORD Reserved3;
  DWORD MxCsr;
  DWORD MxCsr_Mask;
  M128A FloatRegisters[8];
  M128A XmmRegisters[16];
  BYTE Reserved4[96];
} XSAVE_FORMAT, XMM_SAVE_AREA32;

typedef XSAVE_FORMAT *PXMM_SAVE_AREA32;

#define CONTEXT_AMD64   0x100000
#define CONTEXT_CONTROL (CONTEXT_AMD64 | 0x01)
#define CONTEXT_INTEGER (CONTEXT_AMD64 | 0x02)
#define CONTEXT_SEGMENTS (CONTEXT_AMD64 | 0x04)
#define CONTEXT_FLOATING_POINT (CONTEXT_AMD64 | 0x08)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x10)
#define CONTEXT_XSTATE  (CONTEXT_AMD64 | 0x40)
#define CONTEXT_FULL    (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)

typedef struct __attribute__((aligned(16))) _CONTEXT {
  DWORD64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
  DWORD ContextFlags;
  DWORD MxCsr;
  WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
  DWORD EFlags;
  DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
  DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
  DWORD64 R8, R9, R10, R11, R12, R13, R14, R15;
  DWORD64 Rip;
  union {
    XMM_SAVE_AREA32 FltSave;
    struct {
      M128A Header[2];
      M128A Legacy[8];
      M128A Xmm0, Xmm1, Xmm2, Xmm3, Xmm4, Xmm5, Xmm6, Xmm7;
      M128A Xmm8, Xmm9, Xmm10, Xmm11, Xmm12, Xmm13, Xmm14, Xmm15;
    };
  };
  M128A VectorRegister[26];
  DWORD64 VectorControl;
  DWORD64 DebugControl;
  DWORD64 LastBranchToRip, LastBranchFromRip;
  DWORD64 LastExceptionToRip, LastExceptionFromRip;
} CONTEXT;

// Extended context descriptor, follows CONTEXT in memory when
// CONTEXT_XSTATE is used. Created by InitializeContext2.
typedef struct _CONTEXT_CHUNK {
  LONG Offset;
  DWORD Length;
} CONTEXT_CHUNK;

typedef struct _CONTEXT_EX {
  CONTEXT_CHUNK All;     // Entire buffer (CONTEXT + CONTEXT_EX + XSTATE)
  CONTEXT_CHUNK Legacy;  // Legacy CONTEXT region
  CONTEXT_CHUNK XState;  // XSAVE area (AVX/AVX-512)
} CONTEXT_EX;

typedef struct _RUNTIME_FUNCTION {
  DWORD BeginAddress;
  DWORD EndAddress;
  union {
    DWORD UnwindInfoAddress;
    DWORD UnwindData;
  };
} RUNTIME_FUNCTION;

#elif defined(__aarch64__) || defined(_M_ARM64)

typedef union _ARM64_NT_NEON128 {
  struct {
    ULONGLONG Low;
    LONGLONG High;
  };
  double D[2];
  float S[4];
  WORD H[8];
  BYTE B[16];
} ARM64_NT_NEON128;

#define CONTEXT_ARM64   0x00400000
#define CONTEXT_CONTROL (CONTEXT_ARM64 | 0x01)
#define CONTEXT_INTEGER (CONTEXT_ARM64 | 0x02)
#define CONTEXT_FLOATING_POINT (CONTEXT_ARM64 | 0x04)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_ARM64 | 0x08)
#define CONTEXT_ARM64_X18 (CONTEXT_ARM64 | 0x10)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)

#define ARM64_MAX_BREAKPOINTS 8
#define ARM64_MAX_WATCHPOINTS 2

typedef struct __attribute__((aligned(16))) _CONTEXT {
  DWORD ContextFlags;
  DWORD Cpsr;
  union {
    struct {
      DWORD64 X0, X1, X2, X3, X4, X5, X6, X7;
      DWORD64 X8, X9, X10, X11, X12, X13, X14, X15;
      DWORD64 X16, X17, X18, X19, X20, X21, X22, X23;
      DWORD64 X24, X25, X26, X27, X28;
      DWORD64 Fp;
      DWORD64 Lr;
    };
    DWORD64 X[31];
  };
  DWORD64 Sp;
  DWORD64 Pc;
  ARM64_NT_NEON128 V[32];
  DWORD Fpcr;
  DWORD Fpsr;
  DWORD Bcr[ARM64_MAX_BREAKPOINTS];
  DWORD64 Bvr[ARM64_MAX_BREAKPOINTS];
  DWORD Wcr[ARM64_MAX_WATCHPOINTS];
  DWORD64 Wvr[ARM64_MAX_WATCHPOINTS];
} CONTEXT;

typedef struct _RUNTIME_FUNCTION {
  DWORD BeginAddress;
  DWORD EndAddress;
  union {
    DWORD UnwindInfoAddress;
    DWORD UnwindData;
  };
} RUNTIME_FUNCTION;

#else
#error "sys/ntabi.h: unsupported architecture"
#endif

#ifdef __cplusplus
static_assert(sizeof(CONTEXT) ==
#if defined(__x86_64__) || defined(_M_X64)
    1232
#elif defined(__aarch64__) || defined(_M_ARM64)
    912
#endif
    , "CONTEXT size must match Windows SDK");
// Architecture-specific CONTEXT field offsets and supporting struct sizes.
#if defined(__x86_64__) || defined(_M_X64)
// M128A and XSAVE_FORMAT underlie the FltSave / floating-point union.
static_assert(sizeof(M128A) == 16,
              "M128A must be 16 bytes");
static_assert(sizeof(XSAVE_FORMAT) == 512,
              "XSAVE_FORMAT must be 512 bytes");
static_assert(__builtin_offsetof(XSAVE_FORMAT, FloatRegisters) == 32,
              "XSAVE_FORMAT::FloatRegisters must be at offset 32");
static_assert(__builtin_offsetof(XSAVE_FORMAT, XmmRegisters) == 160,
              "XSAVE_FORMAT::XmmRegisters must be at offset 160");
// Integer registers: Rax follows six debug registers.
static_assert(__builtin_offsetof(CONTEXT, Rax) == 120,
              "CONTEXT::Rax must be at offset 120");
static_assert(__builtin_offsetof(CONTEXT, Rip) == 248,
              "CONTEXT::Rip must be at offset 248");
// FltSave union begins immediately after Rip.
static_assert(__builtin_offsetof(CONTEXT, FltSave) == 256,
              "CONTEXT::FltSave must be at offset 256");
// VectorRegister[26] follows the 512-byte floating-point save area.
static_assert(__builtin_offsetof(CONTEXT, VectorRegister) == 768,
              "CONTEXT::VectorRegister must be at offset 768");
// CONTEXT_CHUNK and CONTEXT_EX describe the extended-context buffer layout.
static_assert(sizeof(CONTEXT_CHUNK) == 8,
              "CONTEXT_CHUNK must be 8 bytes");
static_assert(sizeof(CONTEXT_EX) == 24,
              "CONTEXT_EX must be 24 bytes");
#elif defined(__aarch64__) || defined(_M_ARM64)
// ARM64: 31-wide integer file at 8, Sp/Pc follow, then NEON vector state.
static_assert(__builtin_offsetof(CONTEXT, Sp) == 256,
              "CONTEXT::Sp must be at offset 256");
static_assert(__builtin_offsetof(CONTEXT, Pc) == 264,
              "CONTEXT::Pc must be at offset 264");
static_assert(__builtin_offsetof(CONTEXT, V) == 272,
              "CONTEXT::V must be at offset 272");
static_assert(__builtin_offsetof(CONTEXT, Fpcr) == 784,
              "CONTEXT::Fpcr must be at offset 784");
#endif
// RUNTIME_FUNCTION is three DWORDs on both x64 and arm64.
static_assert(sizeof(RUNTIME_FUNCTION) == 12,
              "RUNTIME_FUNCTION must be 12 bytes");
#endif

typedef CONTEXT *PCONTEXT;
typedef RUNTIME_FUNCTION *PRUNTIME_FUNCTION;

//===----------------------------------------------------------------------===//
// EXCEPTION_POINTERS
//===----------------------------------------------------------------------===//

typedef struct _EXCEPTION_POINTERS {
  EXCEPTION_RECORD *ExceptionRecord;
  CONTEXT *ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

//===----------------------------------------------------------------------===//
// VEH Handler Callback
//===----------------------------------------------------------------------===//

// In C++, `[[gnu::ms_abi]]` can decorate a function *declaration* but not
// a function *type* — GCC rejects the trailing-alias form and Clang warns
// under `-Wgcc-compat`. We synthesize the function type via `decltype` on
// an unreferenced external prototype; never defined, never odr-used (the
// operand of `decltype` is unevaluated), so the linker drops it silently.
// In C, `decltype` doesn't exist, but GCC's own `__attribute__((ms_abi))`
// is accepted on types, so the legacy trailing-attribute form stays.
#ifdef __cplusplus
[[gnu::ms_abi]] LONG __vectored_exception_handler_type_source(
    EXCEPTION_POINTERS *);
using _PVECTORED_EXCEPTION_HANDLER_FN =
    decltype(__vectored_exception_handler_type_source);
#else
typedef LONG _PVECTORED_EXCEPTION_HANDLER_FN(EXCEPTION_POINTERS *)
    __attribute__((ms_abi));
#endif
typedef _PVECTORED_EXCEPTION_HANDLER_FN *PVECTORED_EXCEPTION_HANDLER;

//===----------------------------------------------------------------------===//
// DISPATCHER_CONTEXT — per-frame state during SEH dispatch
//===----------------------------------------------------------------------===//

// Same `decltype`-on-prototype pattern as _PVECTORED_EXCEPTION_HANDLER_FN
// above. C keeps the trailing GNU-attribute form.
#ifdef __cplusplus
[[gnu::ms_abi]] EXCEPTION_DISPOSITION __exception_routine_type_source(
    EXCEPTION_RECORD *, PVOID, CONTEXT *, PVOID);
using EXCEPTION_ROUTINE = decltype(__exception_routine_type_source);
#else
typedef EXCEPTION_DISPOSITION EXCEPTION_ROUTINE(EXCEPTION_RECORD *, PVOID,
                                                CONTEXT *, PVOID)
    __attribute__((ms_abi));
#endif
typedef EXCEPTION_ROUTINE *PEXCEPTION_ROUTINE;

// RtlVirtualUnwind handler flags.
#define UNW_FLAG_NHANDLER  0x0
#define UNW_FLAG_EHANDLER  0x1
#define UNW_FLAG_UHANDLER  0x2
#define UNW_FLAG_CHAININFO 0x4

#if defined(__x86_64__) || defined(_M_X64)
typedef struct _DISPATCHER_CONTEXT {
  ULONG64 ControlPc;
  ULONG64 ImageBase;
  PRUNTIME_FUNCTION FunctionEntry;
  ULONG64 EstablisherFrame;
  ULONG64 TargetIp;
  CONTEXT *ContextRecord;
  PEXCEPTION_ROUTINE LanguageHandler;
  PVOID HandlerData;
  PVOID HistoryTable; // UNWIND_HISTORY_TABLE *
  DWORD ScopeIndex;
  DWORD Fill0;
} DISPATCHER_CONTEXT;
#elif defined(__aarch64__) || defined(_M_ARM64)
typedef struct _DISPATCHER_CONTEXT {
  ULONG64 ControlPc;
  ULONG64 ImageBase;
  PRUNTIME_FUNCTION FunctionEntry;
  ULONG64 EstablisherFrame;
  ULONG64 TargetIp;
  CONTEXT *ContextRecord;
  PEXCEPTION_ROUTINE LanguageHandler;
  PVOID HandlerData;
  PVOID HistoryTable; // UNWIND_HISTORY_TABLE *
  DWORD ScopeIndex;
  BOOLEAN ControlPcIsUnwound;
  BYTE *NonVolatileRegisters;
} DISPATCHER_CONTEXT;
#endif

typedef DISPATCHER_CONTEXT *PDISPATCHER_CONTEXT;

//===----------------------------------------------------------------------===//
// UNWIND_HISTORY_TABLE — RtlLookupFunctionEntry cache
//===----------------------------------------------------------------------===//

#define UNWIND_HISTORY_TABLE_SIZE 12

typedef struct _UNWIND_HISTORY_TABLE_ENTRY {
  ULONG64 ImageBase;
  PRUNTIME_FUNCTION FunctionEntry;
} UNWIND_HISTORY_TABLE_ENTRY;

typedef struct _UNWIND_HISTORY_TABLE {
  DWORD Count;
  BYTE LocalHint;
  BYTE GlobalHint;
  BYTE Search;
  BYTE Once;
  ULONG64 LowAddress;
  ULONG64 HighAddress;
  UNWIND_HISTORY_TABLE_ENTRY Entry[UNWIND_HISTORY_TABLE_SIZE];
} UNWIND_HISTORY_TABLE;

typedef UNWIND_HISTORY_TABLE *PUNWIND_HISTORY_TABLE;

//===----------------------------------------------------------------------===//
// NT Runtime Functions (ntdll.dll)
//===----------------------------------------------------------------------===//

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp,
            PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
            CONTEXT *OriginalContext, PVOID HistoryTable);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PRUNTIME_FUNCTION
RtlLookupFunctionEntry(DWORD64 ControlPc, DWORD64 *ImageBase,
                        PVOID HistoryTable);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PEXCEPTION_ROUTINE
RtlVirtualUnwind(DWORD HandlerType, DWORD64 ImageBase, DWORD64 ControlPc,
                 PRUNTIME_FUNCTION FunctionEntry, CONTEXT *ContextRecord,
                 PVOID *HandlerData, DWORD64 *EstablisherFrame,
                 PVOID ContextPointers);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlCaptureContext(CONTEXT *ContextRecord);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlRestoreContext(CONTEXT *ContextRecord, EXCEPTION_RECORD *ExceptionRecord);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlRaiseException(EXCEPTION_RECORD *ExceptionRecord);

// Vectored exception handler registration (ntdll.dll). kernel32's
// Add/RemoveVectoredExceptionHandler are direct PE forwarders to these.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID
RtlAddVectoredExceptionHandler(ULONG First,
                               PVECTORED_EXCEPTION_HANDLER Handler);
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR ULONG
RtlRemoveVectoredExceptionHandler(PVOID Handle);

#ifdef __cplusplus
// ABI layout validation for SEH, VEH, and unwind structures.
// EXCEPTION_RECORD: code/flags at 0-7, chained record pointer and address at
// 8/16, parameter count at 24, then 4 bytes of pad before the 15-element array.
static_assert(sizeof(EXCEPTION_RECORD) == 152,
              "EXCEPTION_RECORD must be 152 bytes");
static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionAddress) == 16,
              "EXCEPTION_RECORD::ExceptionAddress must be at offset 16");
static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionInformation) == 32,
              "EXCEPTION_RECORD::ExceptionInformation must be at offset 32");
// EXCEPTION_POINTERS is two adjacent pointers.
static_assert(sizeof(EXCEPTION_POINTERS) == 16,
              "EXCEPTION_POINTERS must be 16 bytes");
// DISPATCHER_CONTEXT layout differs between architectures.
#if defined(__x86_64__) || defined(_M_X64)
// x64: nine pointer-width fields, ScopeIndex, Fill0 — no trailing pointer.
static_assert(sizeof(DISPATCHER_CONTEXT) == 80,
              "DISPATCHER_CONTEXT must be 80 bytes (x64)");
#elif defined(__aarch64__) || defined(_M_ARM64)
// arm64: same nine pointer fields, ScopeIndex, ControlPcIsUnwound, then a
// trailing BYTE* for NonVolatileRegisters — adds one more pointer slot.
static_assert(sizeof(DISPATCHER_CONTEXT) == 88,
              "DISPATCHER_CONTEXT must be 88 bytes (arm64)");
#endif
// UNWIND_HISTORY_TABLE: 8-byte header (Count + four BYTEs), two ULONG64
// address fields, then 12 entries of 16 bytes each.
static_assert(sizeof(UNWIND_HISTORY_TABLE_ENTRY) == 16,
              "UNWIND_HISTORY_TABLE_ENTRY must be 16 bytes");
static_assert(sizeof(UNWIND_HISTORY_TABLE) == 216,
              "UNWIND_HISTORY_TABLE must be 216 bytes");
static_assert(__builtin_offsetof(UNWIND_HISTORY_TABLE, LowAddress) == 8,
              "UNWIND_HISTORY_TABLE::LowAddress must be at offset 8");
static_assert(__builtin_offsetof(UNWIND_HISTORY_TABLE, Entry) == 24,
              "UNWIND_HISTORY_TABLE::Entry must be at offset 24");
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SYS_NTABI_H
