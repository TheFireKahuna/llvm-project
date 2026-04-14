//===-- PEB / TEB — Process and Thread Environment Blocks --- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Full PEB and TEB layout definitions for Windows 11 24H2.
//
// TEB access: gs:0x30 (x64) / x18 (AArch64) points to the TEB.
// PEB access: TEB offset +0x60 (x64) / +0x60 (AArch64).
//
// Reference: System Informer phnt/include/ntpebteb.h
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PEB_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PEB_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

// FIELD_OFFSET is defined in nt_types.h.

//===----------------------------------------------------------------------===//
// Forward declarations — opaque types referenced by pointer only
//===----------------------------------------------------------------------===//

// User32 kernel callback dispatch table (ntuser.h). Full struct has ~140
// function pointers; PEB only stores a pointer.
typedef struct _KERNEL_CALLBACK_TABLE KERNEL_CALLBACK_TABLE,
    *PKERNEL_CALLBACK_TABLE;

// OLE per-thread state flags (combase.dll internal, TEB::ReservedForOle).
#define OLETLS_LOCALTID 0x00000001
#define OLETLS_UUIDINITIALIZED 0x00000002
#define OLETLS_INTHREADDETACH 0x00000004
#define OLETLS_CHANNELTHREADINITIALZED 0x00000008
#define OLETLS_WOWTHREAD 0x00000010
#define OLETLS_THREADUNINITIALIZING 0x00000020
#define OLETLS_DISABLE_OLE1DDE 0x00000040
#define OLETLS_APARTMENTTHREADED 0x00000080 // STA
#define OLETLS_MULTITHREADED 0x00000100     // MTA
#define OLETLS_IMPERSONATING 0x00000200
#define OLETLS_DISABLE_EVENTLOGGER 0x00000400
#define OLETLS_INNEUTRALAPT 0x00000800 // NTA
#define OLETLS_DISPATCHTHREAD 0x00001000
#define OLETLS_HOSTTHREAD 0x00002000
#define OLETLS_ALLOWCOINIT 0x00004000
#define OLETLS_PENDINGUNINIT 0x00008000
#define OLETLS_FIRSTMTAINIT 0x00010000
#define OLETLS_FIRSTNTAINIT 0x00020000
#define OLETLS_APTINITIALIZING 0x00040000
#define OLETLS_UIMSGSINMODALLOOP 0x00080000
#define OLETLS_MARSHALING_ERROR_OBJECT 0x00100000 // since WIN8
#define OLETLS_WINRT_INITIALIZE 0x00200000        // RoInitialize called
#define OLETLS_APPLICATION_STA 0x00400000
#define OLETLS_IN_SHUTDOWN_CALLBACKS 0x00800000
#define OLETLS_POINTER_INPUT_BLOCKED 0x01000000
#define OLETLS_IN_ACTIVATION_FILTER 0x02000000 // since WINBLUE
#define OLETLS_ASTATOASTAEXEMPT_QUIRK 0x04000000
#define OLETLS_ASTATOASTAEXEMPT_PROXY 0x08000000
#define OLETLS_ASTATOASTAEXEMPT_INDOUBT 0x10000000
#define OLETLS_DETECTED_USER_INITIALIZED 0x20000000 // since RS3
#define OLETLS_BRIDGE_STA 0x40000000                // since RS5
#define OLETLS_NAINITIALIZING 0x80000000UL          // since 19H1

// OLE per-thread state (combase.dll internal). TEB::ReservedForOle points here.
// Only the stable prefix is decoded; later fields are version-dependent.
typedef struct tagSOleTlsData {
  PVOID ThreadBase;      // Per-thread base pointer
  PVOID SmAllocator;     // Docfile allocator
  DWORD ApartmentID;     // COM apartment ID
  DWORD Flags;           // OLETLS_* flags
  LONG TlsMapIndex;      // Index in global TLSMap
  PVOID *TlsSlot;        // Back-pointer to TLS slot
  DWORD ComInits;        // CoInitialize count
  DWORD OleInits;        // OleInitialize count
  DWORD Calls;           // Outstanding call count
  PVOID ServerCall;      // CallInfo (renamed TH1+)
  PVOID CallObjectCache; // FreeAsyncCall (renamed TH1+)
  PVOID ContextStack;    // FreeClientCall (renamed TH1+)
  PVOID ObjServer;       // Apartment activation server
  DWORD TIDCaller;       // TID of current caller
} SOleTlsData, *PSOleTlsData;

// win32k internal types — only used as opaque pointers in CLIENTINFO.
struct tagDESKTOPINFO;
struct tagCLIENTTHREADINFO;

//===----------------------------------------------------------------------===//
// Activation Context — SxS (Side-by-Side) assembly resolution
//===----------------------------------------------------------------------===//
//
// Activation contexts handle DLL redirection, COM registration, and window
// class isolation via XML manifests embedded in PE resources. The kernel
// maintains per-thread and per-process activation context stacks. Decoding
// these structures enables direct manifest queries and DLL redirection
// lookups without QueryActCtxW.

// Requested run level from the manifest's trustInfo element.
typedef enum _ACTCTX_REQUESTED_RUN_LEVEL {
  ACTCTX_RUN_LEVEL_UNSPECIFIED = 0,
  ACTCTX_RUN_LEVEL_AS_INVOKER,
  ACTCTX_RUN_LEVEL_HIGHEST_AVAILABLE,
  ACTCTX_RUN_LEVEL_REQUIRE_ADMIN,
  ACTCTX_RUN_LEVEL_NUMBERS
} ACTCTX_REQUESTED_RUN_LEVEL;

// Serialized activation context data header. Offsets are relative to this
// struct's base address. The ACTIVATION_CONTEXT_DATA is typically mapped
// as a memory section shared between the loader and SxS resolver.
typedef struct _ACTIVATION_CONTEXT_DATA {
  ULONG Magic;
  ULONG HeaderSize;
  ULONG FormatVersion;
  ULONG TotalSize;
  ULONG DefaultTocOffset;  // → ACTIVATION_CONTEXT_DATA_TOC_HEADER
  ULONG ExtendedTocOffset; // → ACTIVATION_CONTEXT_DATA_EXTENDED_TOC_HEADER
  ULONG
      AssemblyRosterOffset; // → ACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_HEADER
  ULONG Flags;              // ACTIVATION_CONTEXT_FLAG_*
} ACTIVATION_CONTEXT_DATA, *PACTIVATION_CONTEXT_DATA;

// Table-of-contents header — indexes section data within the context.
typedef struct _ACTIVATION_CONTEXT_DATA_TOC_HEADER {
  ULONG HeaderSize;
  ULONG EntryCount;
  ULONG FirstEntryOffset; // → ACTIVATION_CONTEXT_DATA_TOC_ENTRY[]
  ULONG Flags;
} ACTIVATION_CONTEXT_DATA_TOC_HEADER, *PACTIVATION_CONTEXT_DATA_TOC_HEADER;

// Extended TOC header — additional section data.
typedef struct _ACTIVATION_CONTEXT_DATA_EXTENDED_TOC_HEADER {
  ULONG HeaderSize;
  ULONG EntryCount;
  ULONG FirstEntryOffset; // → ACTIVATION_CONTEXT_DATA_EXTENDED_TOC_ENTRY[]
  ULONG Flags;
} ACTIVATION_CONTEXT_DATA_EXTENDED_TOC_HEADER,
    *PACTIVATION_CONTEXT_DATA_EXTENDED_TOC_HEADER;

// Assembly roster header — enumerates all assemblies in the context.
typedef struct _ACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_HEADER {
  ULONG HeaderSize;
  ULONG HashAlgorithm; // HASH_STRING_ALGORITHM_*
  ULONG EntryCount;
  ULONG FirstEntryOffset; // → ACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_ENTRY[]
  ULONG AssemblyInformationSectionOffset;
} ACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_HEADER,
    *PACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_HEADER;

// Per-assembly roster entry — links name and info for one assembly.
typedef struct _ACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_ENTRY {
  ULONG Flags;
  ULONG PseudoKey;
  ULONG AssemblyNameOffset; // → WCHAR[], from context data base
  ULONG AssemblyNameLength;
  ULONG
      AssemblyInformationOffset; // →
                                 // ACTIVATION_CONTEXT_DATA_ASSEMBLY_INFORMATION
  ULONG AssemblyInformationLength;
} ACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_ENTRY,
    *PACTIVATION_CONTEXT_DATA_ASSEMBLY_ROSTER_ENTRY;

// Per-assembly metadata — manifest path, policy, version, run level.
typedef struct _ACTIVATION_CONTEXT_DATA_ASSEMBLY_INFORMATION {
  ULONG Size;
  ULONG Flags;
  ULONG EncodedAssemblyIdentityLength;
  ULONG EncodedAssemblyIdentityOffset; // → WCHAR[], from section header
  ULONG ManifestPathType;              // ACTIVATION_CONTEXT_PATH_TYPE_*
  ULONG ManifestPathLength;
  ULONG ManifestPathOffset; // → WCHAR[], from section header
  struct {
    ULONG LowPart;
    LONG HighPart;
  } ManifestLastWriteTime;
  ULONG PolicyPathType;
  ULONG PolicyPathLength;
  ULONG PolicyPathOffset; // → WCHAR[], from section header
  struct {
    ULONG LowPart;
    LONG HighPart;
  } PolicyLastWriteTime;
  ULONG MetadataSatelliteRosterIndex;
  ULONG Unused2;
  ULONG ManifestVersionMajor;
  ULONG ManifestVersionMinor;
  ULONG PolicyVersionMajor;
  ULONG PolicyVersionMinor;
  ULONG AssemblyDirectoryNameLength;
  ULONG AssemblyDirectoryNameOffset; // → WCHAR[], from section header
  ULONG NumOfFilesInAssembly;
  ULONG LanguageLength;
  ULONG LanguageOffset; // → WCHAR[], from section header
  ACTCTX_REQUESTED_RUN_LEVEL RunLevel;
  ULONG UiAccess;
} ACTIVATION_CONTEXT_DATA_ASSEMBLY_INFORMATION,
    *PACTIVATION_CONTEXT_DATA_ASSEMBLY_INFORMATION;

// Assembly storage map — SxS per-assembly path resolution cache.
// Defined here (before ACTIVATION_CONTEXT which embeds it directly).
typedef struct _ASSEMBLY_STORAGE_MAP_ENTRY {
  ULONG Flags;
  UNICODE_STRING DosPath;
  HANDLE Handle;
} ASSEMBLY_STORAGE_MAP_ENTRY, *PASSEMBLY_STORAGE_MAP_ENTRY;

typedef struct _ASSEMBLY_STORAGE_MAP {
  ULONG Flags;
  ULONG AssemblyCount;
  PASSEMBLY_STORAGE_MAP_ENTRY *AssemblyArray;
} ASSEMBLY_STORAGE_MAP, *PASSEMBLY_STORAGE_MAP;

// Notification callback — invoked when activation contexts are
// activated/deactivated. The _Function_class_ SAL annotation is omitted.
typedef void(NTAPI *PACTIVATION_CONTEXT_NOTIFY_ROUTINE)(
    ULONG NotificationType, // ACTIVATION_CONTEXT_NOTIFICATION_*
    struct _ACTIVATION_CONTEXT *ActivationContext,
    PACTIVATION_CONTEXT_DATA ActivationContextData, PVOID NotificationContext,
    PVOID NotificationData, BOOLEAN *DisableThisNotification);

// ACTIVATION_CONTEXT — runtime state for one activation context.
typedef struct _ACTIVATION_CONTEXT {
  LONG RefCount;
  ULONG Flags;
  PACTIVATION_CONTEXT_DATA ActivationContextData;
  PACTIVATION_CONTEXT_NOTIFY_ROUTINE NotificationRoutine;
  PVOID NotificationContext;
  ULONG SentNotifications[8];
  ULONG DisabledNotifications[8];
  ASSEMBLY_STORAGE_MAP StorageMap;
  PASSEMBLY_STORAGE_MAP_ENTRY InlineStorageMapEntries[32];
} ACTIVATION_CONTEXT;
// Note: PACTIVATION_CONTEXT is forward-declared in nt_types.h.

// Activation context stack frame — one entry per pushed context.
typedef struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME {
  struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME *Previous;
  PACTIVATION_CONTEXT ActivationContext;
  ULONG Flags;
} RTL_ACTIVATION_CONTEXT_STACK_FRAME, *PRTL_ACTIVATION_CONTEXT_STACK_FRAME;

// Activation context stack — embedded in the TEB (not a pointer).
// sizeof = 0x28 on x64.
typedef struct _ACTIVATION_CONTEXT_STACK {
  PRTL_ACTIVATION_CONTEXT_STACK_FRAME ActiveFrame;
  LIST_ENTRY FrameListCache;
  ULONG Flags;
  ULONG NextCookieSequenceNumber;
  ULONG StackId;
} ACTIVATION_CONTEXT_STACK, *PACTIVATION_CONTEXT_STACK;

//===----------------------------------------------------------------------===//
// NT_TIB — Thread Information Block (x64)
//===----------------------------------------------------------------------===//
//
// The first field of every TEB. Contains the SEH exception chain, stack
// bounds, and a self-pointer. gs:0x00 on x64, x18+0x00 on AArch64.

struct EXCEPTION_REGISTRATION_RECORD {
  EXCEPTION_REGISTRATION_RECORD *Next;
  PVOID Handler; // PEXCEPTION_ROUTINE
};

struct NT_TIB {
  EXCEPTION_REGISTRATION_RECORD *ExceptionList; // SEH chain head
  PVOID StackBase;                              // High address (grows down)
  PVOID StackLimit;                             // Low address (guard page)
  PVOID SubSystemTib;
  union {
    PVOID FiberData; // Non-null when running in a fiber
    ULONG Version;
  };
  PVOID ArbitraryUserPointer;
  NT_TIB *Self; // gs:0x30 — pointer back to TEB
};

//===----------------------------------------------------------------------===//
// GUID — 128-bit Globally Unique Identifier
//===----------------------------------------------------------------------===//

struct GUID {
  ULONG Data1;
  USHORT Data2;
  USHORT Data3;
  UCHAR Data4[8];
};

//===----------------------------------------------------------------------===//
// POINT — 2D coordinate (used by MSG in the TEB)
//===----------------------------------------------------------------------===//

struct POINT {
  LONG x;
  LONG y;
};

//===----------------------------------------------------------------------===//
// RTL_BITMAP — bit array used for TLS slot tracking
//===----------------------------------------------------------------------===//

typedef struct _RTL_BITMAP {
  ULONG SizeOfBitMap;
  PULONG Buffer;
} RTL_BITMAP, *PRTL_BITMAP;

//===----------------------------------------------------------------------===//
// CURDIR — current directory state in RTL_USER_PROCESS_PARAMETERS
//===----------------------------------------------------------------------===//

struct CURDIR {
  UNICODE_STRING DosPath;
  HANDLE Handle;
};

//===----------------------------------------------------------------------===//
// RTL_DRIVE_LETTER_CURDIR — per-drive current directory (A: through Z:+6)
//===----------------------------------------------------------------------===//

#define RTL_MAX_DRIVE_LETTERS 32
#define RTL_DRIVE_LETTER_VALID (USHORT)0x0001

typedef struct _RTL_DRIVE_LETTER_CURDIR {
  USHORT Flags;
  USHORT Length;
  ULONG TimeStamp;
  STRING DosPath;
} RTL_DRIVE_LETTER_CURDIR, *PRTL_DRIVE_LETTER_CURDIR;

//===----------------------------------------------------------------------===//
// RTL_USER_PROCESS_PARAMETERS — process startup parameters
//===----------------------------------------------------------------------===//
//
// Allocated in the new process address space by RtlCreateProcessParametersEx.
// Contains image path, command line, environment block, console handles,
// current directory, standard I/O handles, and window creation info.

typedef struct _RTL_USER_PROCESS_PARAMETERS {
  ULONG MaximumLength;
  ULONG Length;

  ULONG Flags;
  ULONG DebugFlags;

  HANDLE ConsoleHandle;
  ULONG ConsoleFlags;
  HANDLE StandardInput;
  HANDLE StandardOutput;
  HANDLE StandardError;

  CURDIR CurrentDirectory;
  UNICODE_STRING DllPath;
  UNICODE_STRING ImagePathName;
  UNICODE_STRING CommandLine;
  PVOID Environment;

  ULONG StartingX;
  ULONG StartingY;
  ULONG CountX;
  ULONG CountY;
  ULONG CountCharsX;
  ULONG CountCharsY;
  ULONG FillAttribute;

  ULONG WindowFlags;
  ULONG ShowWindowFlags;
  UNICODE_STRING WindowTitle;
  UNICODE_STRING DesktopInfo;
  UNICODE_STRING ShellInfo;
  UNICODE_STRING RuntimeData;
  RTL_DRIVE_LETTER_CURDIR CurrentDirectories[RTL_MAX_DRIVE_LETTERS];

  ULONG_PTR EnvironmentSize;
  ULONG_PTR EnvironmentVersion;

  PVOID PackageDependencyData;
  ULONG ProcessGroupId;
  ULONG LoaderThreads;               // since THRESHOLD
  UNICODE_STRING RedirectionDllName; // since REDSTONE5
  UNICODE_STRING HeapPartitionName;  // since 19H1
  PULONGLONG DefaultThreadpoolCpuSetMasks;
  ULONG DefaultThreadpoolCpuSetMaskCount;
  ULONG DefaultThreadpoolThreadMaximum; // since 20H1
  ULONG HeapMemoryTypeMask;             // since WIN11 22H2
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

inline constexpr ULONG RTL_USER_PROC_PARAMS_NORMALIZED = 0x00000001;

//===----------------------------------------------------------------------===//
// PEB_LDR_DATA — loaded module list (accessed via PEB::Ldr)
//===----------------------------------------------------------------------===//

typedef struct _PEB_LDR_DATA {
  ULONG Length;
  BOOLEAN Initialized;
  HANDLE SsHandle;
  LIST_ENTRY InLoadOrderModuleList;
  LIST_ENTRY InMemoryOrderModuleList;
  LIST_ENTRY InInitializationOrderModuleList;
  PVOID EntryInProgress;
  BOOLEAN ShutdownInProgress;
  HANDLE ShutdownThreadId;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

//===----------------------------------------------------------------------===//
// RTL_CRITICAL_SECTION — user-mode recursive lock (PEB::FastPebLock, etc.)
//===----------------------------------------------------------------------===//

typedef struct _RTL_CRITICAL_SECTION_DEBUG {
  USHORT Type;
  USHORT CreatorBackTraceIndex;
  struct _RTL_CRITICAL_SECTION *CriticalSection;
  LIST_ENTRY ProcessLocksList;
  ULONG EntryCount;
  ULONG ContentionCount;
  ULONG Flags;
  USHORT CreatorBackTraceIndexHigh;
  USHORT Identifier;
} RTL_CRITICAL_SECTION_DEBUG, *PRTL_CRITICAL_SECTION_DEBUG, RTL_RESOURCE_DEBUG,
    *PRTL_RESOURCE_DEBUG;

typedef struct _RTL_CRITICAL_SECTION {
  PRTL_CRITICAL_SECTION_DEBUG DebugInfo;
  LONG LockCount;
  LONG RecursionCount;
  HANDLE OwningThread;
  HANDLE LockSemaphore;
  SIZE_T SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;

typedef struct _RTL_RESOURCE {
  RTL_CRITICAL_SECTION CriticalSection;
  HANDLE SharedSemaphore;
  volatile ULONG NumberOfWaitingShared;
  HANDLE ExclusiveSemaphore;
  volatile ULONG NumberOfWaitingExclusive;
  volatile LONG NumberOfActive;
  HANDLE ExclusiveOwnerThread;
  ULONG Flags;
  PRTL_RESOURCE_DEBUG DebugInfo;
} RTL_RESOURCE, *PRTL_RESOURCE;

static_assert(sizeof(RTL_CRITICAL_SECTION) == 0x28,
              "RTL_CRITICAL_SECTION size mismatch");
static_assert(sizeof(RTL_RESOURCE) == 0x60, "RTL_RESOURCE size mismatch");

//===----------------------------------------------------------------------===//
// SLIST_HEADER — lock-free singly-linked list header (PEB::AtlThunkSListPtr)
//===----------------------------------------------------------------------===//

typedef union _SLIST_HEADER {
  union {
    struct {
      ULONGLONG Alignment;
      ULONGLONG Region;
    };
    struct {
      ULONGLONG Depth : 16;
      ULONGLONG Sequence : 48;
    };
    struct {
      ULONGLONG Reserved : 4;
      ULONGLONG NextEntry : 60;
    } HeaderX64;
  };
} SLIST_HEADER, *PSLIST_HEADER;

//===----------------------------------------------------------------------===//
// NT_PRODUCT_TYPE — OS product family (PEB::NtProductType... via SharedData)
//===----------------------------------------------------------------------===//

typedef enum _NT_PRODUCT_TYPE {
  NtProductWinNt = 1, // Workstation
  NtProductLanManNt,  // Domain Controller
  NtProductServer     // Server
} NT_PRODUCT_TYPE,
    *PNT_PRODUCT_TYPE;

//===----------------------------------------------------------------------===//
// KSYSTEM_TIME — interrupt/system time (SharedUserData)
//===----------------------------------------------------------------------===//

typedef struct _KSYSTEM_TIME {
  ULONG LowPart;
  LONG High1Time;
  LONG High2Time;
} KSYSTEM_TIME, *PKSYSTEM_TIME;

//===----------------------------------------------------------------------===//
// SILO_USER_SHARED_DATA — per-silo shared data (PEB::SharedData)
//===----------------------------------------------------------------------===//

typedef struct _SILO_USER_SHARED_DATA {
  ULONG ServiceSessionId;
  ULONG ActiveConsoleId;
  LONGLONG ConsoleSessionForegroundProcessId;
  NT_PRODUCT_TYPE NtProductType;
  ULONG SuiteMask;
  ULONG SharedUserSessionId; // since RS2
  BOOLEAN IsMultiSessionSku;
  BOOLEAN IsStateSeparationEnabled;
  WCHAR NtSystemRoot[260];
  USHORT UserModeGlobalLogger[16];
  ULONG TimeZoneId; // since 21H2
  LONG TimeZoneBiasStamp;
  KSYSTEM_TIME TimeZoneBias;
  LARGE_INTEGER TimeZoneBiasEffectiveStart;
  LARGE_INTEGER TimeZoneBiasEffectiveEnd;
} SILO_USER_SHARED_DATA, *PSILO_USER_SHARED_DATA;

#define API_SET_SECTION_NAME ".apiset"
#define API_SET_SCHEMA_ENTRY_FLAGS_SEALED 0x00000001
#define API_SET_SCHEMA_ENTRY_FLAGS_EXTENSION 0x00000002

#define API_SET_SCHEMA_VERSION_V2 0x00000002 // WIN7, WIN8
#define API_SET_SCHEMA_VERSION_V4 0x00000004 // WINBLUE
#define API_SET_SCHEMA_VERSION_V6 0x00000006 // since THRESHOLD
#define API_SET_SCHEMA_VERSION API_SET_SCHEMA_VERSION_V6

#define API_SET_SCHEMA_FLAGS_SEALED 0x00000001
#define API_SET_SCHEMA_FLAGS_HOST_EXTENSION 0x00000002

// Hash bucket for API Set namespace lookup.
typedef struct _API_SET_HASH_ENTRY {
  ULONG Hash;
  ULONG Index;
} API_SET_HASH_ENTRY, *PAPI_SET_HASH_ENTRY;

// Per-API set namespace entry — maps virtual DLL name to host DLL (V6).
typedef struct _API_SET_NAMESPACE_ENTRY {
  ULONG Flags;      // API_SET_SCHEMA_ENTRY_FLAGS_*
  ULONG NameOffset; // WCHAR[], from schema base
  ULONG NameLength;
  ULONG HashedLength;
  ULONG ValueOffset; // API_SET_VALUE_ENTRY[], from schema base
  ULONG ValueCount;
} API_SET_NAMESPACE_ENTRY, *PAPI_SET_NAMESPACE_ENTRY;

// Namespace entry for V2 schema (Win7/Win8).
typedef struct _API_SET_NAMESPACE_ENTRY_V2 {
  ULONG NameOffset;
  ULONG NameLength;
  ULONG DataOffset; // to API_SET_VALUE_ARRAY_V2
} API_SET_NAMESPACE_ENTRY_V2, *PAPI_SET_NAMESPACE_ENTRY_V2;

// Namespace entry for V4 schema (WinBlue).
typedef struct _API_SET_NAMESPACE_ENTRY_V4 {
  ULONG Flags; // API_SET_SCHEMA_ENTRY_FLAGS_*
  ULONG NameOffset;
  ULONG NameLength;
  ULONG AliasOffset;
  ULONG AliasLength;
  ULONG DataOffset; // to API_SET_VALUE_ARRAY_V4
} API_SET_NAMESPACE_ENTRY_V4, *PAPI_SET_NAMESPACE_ENTRY_V4;

// Value entry — host DLL redirection target (V6).
typedef struct _API_SET_VALUE_ENTRY {
  ULONG Flags;
  ULONG NameOffset; // WCHAR[], from schema base
  ULONG NameLength;
  ULONG ValueOffset; // WCHAR[], from schema base
  ULONG ValueLength;
} API_SET_VALUE_ENTRY, *PAPI_SET_VALUE_ENTRY;

// Value entry for V2 schema.
typedef struct _API_SET_VALUE_ENTRY_V2 {
  ULONG NameOffset;
  ULONG NameLength;
  ULONG ValueOffset;
  ULONG ValueLength;
} API_SET_VALUE_ENTRY_V2, *PAPI_SET_VALUE_ENTRY_V2;

// Value entry for V4 schema.
typedef struct _API_SET_VALUE_ENTRY_V4 {
  ULONG Flags;
  ULONG NameOffset;
  ULONG NameLength;
  ULONG ValueOffset;
  ULONG ValueLength;
} API_SET_VALUE_ENTRY_V4, *PAPI_SET_VALUE_ENTRY_V4;

// Namespace array for V2 schema.
typedef struct _API_SET_NAMESPACE_ARRAY_V2 {
  ULONG Version; // API_SET_SCHEMA_VERSION_V2
  ULONG Count;
  API_SET_NAMESPACE_ENTRY_V2 Array[ANYSIZE_ARRAY];
} API_SET_NAMESPACE_ARRAY_V2, *PAPI_SET_NAMESPACE_ARRAY_V2;

// Namespace array for V4 schema.
typedef struct _API_SET_NAMESPACE_ARRAY_V4 {
  ULONG Version; // API_SET_SCHEMA_VERSION_V4
  ULONG Size;
  ULONG Flags; // API_SET_SCHEMA_FLAGS_*
  ULONG Count;
  API_SET_NAMESPACE_ENTRY_V4 Array[ANYSIZE_ARRAY];
} API_SET_NAMESPACE_ARRAY_V4, *PAPI_SET_NAMESPACE_ARRAY_V4;

// Value array for V2 schema.
typedef struct _API_SET_VALUE_ARRAY_V2 {
  ULONG Count;
  API_SET_VALUE_ENTRY_V2 Array[ANYSIZE_ARRAY];
} API_SET_VALUE_ARRAY_V2, *PAPI_SET_VALUE_ARRAY_V2;

// Value array for V4 schema.
typedef struct _API_SET_VALUE_ARRAY_V4 {
  ULONG Flags;
  ULONG Count;
  API_SET_VALUE_ENTRY_V4 Array[ANYSIZE_ARRAY];
} API_SET_VALUE_ARRAY_V4, *PAPI_SET_VALUE_ARRAY_V4;

//===----------------------------------------------------------------------===//
// API_SET_NAMESPACE — API Set schema (PEB::ApiSetMap, since THRESHOLD)
//===----------------------------------------------------------------------===//

typedef struct _API_SET_NAMESPACE {
  ULONG Version; // API_SET_SCHEMA_VERSION_V6
  ULONG Size;
  ULONG Flags;
  ULONG Count;
  ULONG EntryOffset; // API_SET_NAMESPACE_ENTRY[Count]
  ULONG HashOffset;  // API_SET_HASH_ENTRY[Count]
  ULONG HashFactor;
} API_SET_NAMESPACE, *PAPI_SET_NAMESPACE;

//===----------------------------------------------------------------------===//
// NLS Code Page Tables — PEB::AnsiCodePageData, OemCodePageData
//===----------------------------------------------------------------------===//

#define MAXIMUM_LEADBYTES 12

typedef struct _CPTABLEINFO {
  USHORT CodePage;
  USHORT MaximumCharacterSize;
  USHORT DefaultChar;         // Default character (multibyte)
  USHORT UniDefaultChar;      // Default character (Unicode)
  USHORT TransDefaultChar;    // Translation of default char (Unicode)
  USHORT TransUniDefaultChar; // Translation of Unicode default char (MB)
  USHORT DBCSCodePage;        // Non-zero for DBCS code pages
  UCHAR LeadByte[MAXIMUM_LEADBYTES];
  PUSHORT MultiByteTable;
  PVOID WideCharTable;
  PUSHORT DBCSRanges;
  PUSHORT DBCSOffsets;
} CPTABLEINFO, *PCPTABLEINFO;

typedef struct _NLSTABLEINFO {
  CPTABLEINFO OemTableInfo;
  CPTABLEINFO AnsiTableInfo;
  PUSHORT UpperCaseTable; // 844-format uppercase table
  PUSHORT LowerCaseTable; // 844-format lowercase table
} NLSTABLEINFO, *PNLSTABLEINFO;

//===----------------------------------------------------------------------===//
// GDI handle table entry — PEB::GdiSharedHandleTable
//===----------------------------------------------------------------------===//

typedef struct _GDI_HANDLE_ENTRY {
  union {
    PVOID Object;
    PVOID NextFree;
  };
  union {
    struct {
      USHORT ProcessId;
      USHORT Lock : 1;
      USHORT Count : 15;
    };
    ULONG Value;
  } Owner;
  USHORT Unique;
  UCHAR Type;
  UCHAR Flags;
  PVOID UserPointer;
} GDI_HANDLE_ENTRY, *PGDI_HANDLE_ENTRY;

#define GDI_HANDLE_BUFFER_SIZE32 34
#define GDI_HANDLE_BUFFER_SIZE64 60
#ifdef _WIN64
#define GDI_HANDLE_BUFFER_SIZE GDI_HANDLE_BUFFER_SIZE64
#else
#define GDI_HANDLE_BUFFER_SIZE GDI_HANDLE_BUFFER_SIZE32
#endif
typedef ULONG GDI_HANDLE_BUFFER[GDI_HANDLE_BUFFER_SIZE];

//===----------------------------------------------------------------------===//
// PS_POST_PROCESS_INIT_ROUTINE — PEB::PostProcessInitRoutine callback
//===----------------------------------------------------------------------===//
//
// Optional callback invoked by the loader after process initialization.
// The _Function_class_ SAL annotation is MSVC-specific and omitted here.

typedef void(NTAPI *PPS_POST_PROCESS_INIT_ROUTINE)(void);

//===----------------------------------------------------------------------===//
// Opaque forward declarations for PEB fields used only as pointers
//===----------------------------------------------------------------------===//

typedef struct _HEAP HEAP, *PHEAP;
typedef struct _SHIM_PROCESS_CONTEXT SHIM_PROCESS_CONTEXT,
    *PSHIM_PROCESS_CONTEXT;
typedef struct _LEAP_SECOND_DATA *PLEAP_SECOND_DATA;

// ASSEMBLY_STORAGE_MAP is defined above with the activation context structs.

//===----------------------------------------------------------------------===//
// WER (Windows Error Reporting) — PEB::WerRegistrationData
//===----------------------------------------------------------------------===//

#define RESTART_MAX_CMD_LINE 1024

typedef struct _WER_RECOVERY_INFO {
  ULONG Length;
  PVOID Callback;
  PVOID Parameter;
  HANDLE Started;
  HANDLE Finished;
  HANDLE InProgress;
  LONG LastError;
  BOOL Successful;
  ULONG PingInterval;
  ULONG Flags;
} WER_RECOVERY_INFO, *PWER_RECOVERY_INFO;

typedef struct _WER_FILE {
  USHORT Flags;
  WCHAR Path[MAX_PATH];
} WER_FILE, *PWER_FILE;

typedef struct _WER_MEMORY {
  PVOID Address;
  ULONG Size;
} WER_MEMORY, *PWER_MEMORY;

typedef struct _WER_GATHER {
  PVOID Next;
  USHORT Flags;
  union {
    WER_FILE File;
    WER_MEMORY Memory;
  } v;
} WER_GATHER, *PWER_GATHER;

// WER_GATHER::Flags bit definitions.
inline constexpr USHORT WER_GATHER_FLAG_FILE = 0x0001;
inline constexpr USHORT WER_GATHER_FLAG_MEMORY = 0x0002;
inline constexpr USHORT WER_GATHER_FLAG_EXCLUDE = 0x0004;

typedef struct _WER_METADATA {
  PVOID Next;
  WCHAR Key[64];
  WCHAR Value[128];
} WER_METADATA, *PWER_METADATA;

typedef struct _WER_RUNTIME_DLL {
  PVOID Next;
  ULONG Length;
  PVOID Context;
  WCHAR CallbackDllPath[MAX_PATH];
} WER_RUNTIME_DLL, *PWER_RUNTIME_DLL;

typedef struct _WER_DUMP_COLLECTION {
  PVOID Next;
  ULONG ProcessId;
  ULONG ThreadId;
} WER_DUMP_COLLECTION, *PWER_DUMP_COLLECTION;

typedef struct _WER_HEAP_MAIN_HEADER {
  WCHAR Signature[16];
  LIST_ENTRY Links;
  HANDLE Mutex;
  PVOID FreeHeap;
  ULONG FreeCount;
} WER_HEAP_MAIN_HEADER, *PWER_HEAP_MAIN_HEADER;

typedef struct _WER_PEB_HEADER_BLOCK {
  LONG Length;
  WCHAR Signature[16];
  WCHAR AppDataRelativePath[64];
  WCHAR RestartCommandLine[RESTART_MAX_CMD_LINE];
  WER_RECOVERY_INFO RecoveryInfo;
  PWER_GATHER Gather;
  PWER_METADATA MetaData;
  PWER_RUNTIME_DLL RuntimeDll;
  PWER_DUMP_COLLECTION DumpCollection;
  LONG GatherCount;
  LONG MetaDataCount;
  LONG DumpCount;
  LONG Flags;
  WER_HEAP_MAIN_HEADER MainHeader;
  PVOID Reserved;
} WER_PEB_HEADER_BLOCK, *PWER_PEB_HEADER_BLOCK;

//===----------------------------------------------------------------------===//
// TELEMETRY_COVERAGE_HEADER — PEB::TelemetryCoverageHeader (since RS3)
//===----------------------------------------------------------------------===//

typedef struct _TELEMETRY_COVERAGE_HEADER {
  UCHAR MajorVersion;
  UCHAR MinorVersion;
  struct {
    USHORT TracingEnabled : 1;
    USHORT Reserved1 : 15;
  };
  ULONG HashTableEntries;
  ULONG HashIndexMask;
  ULONG TableUpdateVersion;
  ULONG TableSizeInBytes;
  ULONG LastResetTick;
  ULONG ResetRound;
  ULONG Reserved2;
  ULONG RecordedCount;
  ULONG Reserved3[4];
  ULONG HashTable[ANYSIZE_ARRAY]; // Variable-length
} TELEMETRY_COVERAGE_HEADER, *PTELEMETRY_COVERAGE_HEADER;

//===----------------------------------------------------------------------===//
// DPI context — TEB CLIENTINFO field
//===----------------------------------------------------------------------===//

typedef struct tagDPICONTEXTINFO {
  ULONG dpiContext;
  LOGICAL Dirty;
} DPICONTEXTINFO, *PDPICONTEXTINFO;

//===----------------------------------------------------------------------===//
// PEB — Process Environment Block
//===----------------------------------------------------------------------===//
//
// Offsets marked in comments are for x64. The PEB is version-dependent;
// this layout targets Windows 11 24H2 (sizeof = 0x7d0).
//
// Well-known offsets:
//   +0x002  BeingDebugged
//   +0x020  ProcessParameters
//   +0x030  ProcessHeap
//   +0x058  KernelCallbackTable
//   +0x060  TlsExpansionCounter  (not to be confused with TEB offset 0x60 = PEB
//   pointer) +0x078  TlsBitmap +0x2C0  SessionId

#define PeBeingDebugged 0x2
#define PeProcessParameters 0x20
#define PeKernelCallbackTable 0x58
#define ProcessEnvironmentBlockLength 0x7d0

typedef struct _PEB {
  BOOLEAN InheritedAddressSpace;    // Cloned with inherited address space
  BOOLEAN ReadImageFileExecOptions; // Has IFEO (Image File Execution Options)
  BOOLEAN BeingDebugged;            // Debugger attached

  union {
    BOOLEAN BitField;
    struct {
      BOOLEAN ImageUsesLargePages : 1; // Uses 4 MB image regions
      BOOLEAN IsProtectedProcess : 1;
      BOOLEAN IsImageDynamicallyRelocated : 1;  // ASLR relocated
      BOOLEAN SkipPatchingUser32Forwarders : 1; // 1 for 64-bit, 0 for 32-bit
      BOOLEAN IsPackagedProcess : 1;            // APPX/MSIX store process
      BOOLEAN IsAppContainerProcess : 1;        // Has AppContainer token
      BOOLEAN IsProtectedProcessLight : 1;      // PPL
      BOOLEAN IsLongPathAwareProcess : 1;       // Long path aware
    };
  };

  HANDLE Mutant;                                  // +0x08
  PVOID ImageBaseAddress;                         // +0x10
  PPEB_LDR_DATA Ldr;                              // +0x18
  PRTL_USER_PROCESS_PARAMETERS ProcessParameters; // +0x20
  PVOID SubSystemData;
  PHEAP ProcessHeap;                 // +0x30
  PRTL_CRITICAL_SECTION FastPebLock; // +0x38
  PSLIST_HEADER AtlThunkSListPtr;    // ATL thunk list
  HANDLE IFEOKey;                    // IFEO registry key handle

  union {
    ULONG CrossProcessFlags;
    struct {
      ULONG ProcessInJob : 1;
      ULONG ProcessInitializing : 1;
      ULONG ProcessUsingVEH : 1;
      ULONG ProcessUsingVCH : 1;
      ULONG ProcessUsingFTH : 1;
      ULONG ProcessPreviouslyThrottled : 1;
      ULONG ProcessCurrentlyThrottled : 1;
      ULONG ProcessImagesHotPatched : 1; // since RS5
      ULONG ReservedBits0 : 24;
    };
  };

  // User32 kernel callback dispatch table (ntuser.h)
  union {
    PKERNEL_CALLBACK_TABLE KernelCallbackTable; // +0x58
    PVOID UserSharedInfoPtr;
  };

  ULONG SystemReserved;
  ULONG AtlThunkSListPtr32;     // 32-bit ATL thunk list
  PAPI_SET_NAMESPACE ApiSetMap; // API Set schema
  ULONG TlsExpansionCounter;
  PRTL_BITMAP TlsBitmap; // +0x78
  ULONG TlsBitmapBits[2];

  PVOID ReadOnlySharedMemoryBase;    // CSRSS shared memory
  PSILO_USER_SHARED_DATA SharedData; // Per-silo USER_SHARED_DATA
  PVOID *ReadOnlyStaticServerData;   // CSRSS

  PCPTABLEINFO AnsiCodePageData;
  PCPTABLEINFO OemCodePageData;
  PNLSTABLEINFO UnicodeCaseTableData;

  ULONG NumberOfProcessors;

  union {
    ULONG NtGlobalFlag;
    struct {
      ULONG StopOnException : 1;          // FLG_STOP_ON_EXCEPTION
      ULONG ShowLoaderSnaps : 1;          // FLG_SHOW_LDR_SNAPS
      ULONG DebugInitialCommand : 1;      // FLG_DEBUG_INITIAL_COMMAND
      ULONG StopOnHungGUI : 1;            // FLG_STOP_ON_HUNG_GUI
      ULONG HeapEnableTailCheck : 1;      // FLG_HEAP_ENABLE_TAIL_CHECK
      ULONG HeapEnableFreeCheck : 1;      // FLG_HEAP_ENABLE_FREE_CHECK
      ULONG HeapValidateParameters : 1;   // FLG_HEAP_VALIDATE_PARAMETERS
      ULONG HeapValidateAll : 1;          // FLG_HEAP_VALIDATE_ALL
      ULONG ApplicationVerifier : 1;      // FLG_APPLICATION_VERIFIER
      ULONG MonitorSilentProcessExit : 1; // FLG_MONITOR_SILENT_PROCESS_EXIT
      ULONG PoolEnableTagging : 1;        // FLG_POOL_ENABLE_TAGGING
      ULONG HeapEnableTagging : 1;        // FLG_HEAP_ENABLE_TAGGING
      ULONG UserStackTraceDb : 1;         // FLG_USER_STACK_TRACE_DB
      ULONG KernelStackTraceDb : 1;       // FLG_KERNEL_STACK_TRACE_DB
      ULONG MaintainObjectTypeList : 1;   // FLG_MAINTAIN_OBJECT_TYPELIST
      ULONG HeapEnableTagByDll : 1;       // FLG_HEAP_ENABLE_TAG_BY_DLL
      ULONG DisableStackExtension : 1;    // FLG_DISABLE_STACK_EXTENSION
      ULONG EnableCsrDebug : 1;           // FLG_ENABLE_CSRDEBUG
      ULONG EnableKDebugSymbolLoad : 1;   // FLG_ENABLE_KDEBUG_SYMBOL_LOAD
      ULONG DisablePageKernelStacks : 1;  // FLG_DISABLE_PAGE_KERNEL_STACKS
      ULONG EnableSystemCritBreaks : 1;   // FLG_ENABLE_SYSTEM_CRIT_BREAKS
      ULONG HeapDisableCoalescing : 1;    // FLG_HEAP_DISABLE_COALESCING
      ULONG EnableCloseExceptions : 1;    // FLG_ENABLE_CLOSE_EXCEPTIONS
      ULONG EnableExceptionLogging : 1;   // FLG_ENABLE_EXCEPTION_LOGGING
      ULONG EnableHandleTypeTagging : 1;  // FLG_ENABLE_HANDLE_TYPE_TAGGING
      ULONG HeapPageAllocs : 1;           // FLG_HEAP_PAGE_ALLOCS
      ULONG DebugInitialCommandEx : 1;    // FLG_DEBUG_INITIAL_COMMAND_EX
      ULONG DisableDbgPrint : 1;          // FLG_DISABLE_DBGPRINT
      ULONG CritSecEventCreation : 1;     // FLG_CRITSEC_EVENT_CREATION
      ULONG LdrTopDown : 1;               // FLG_LDR_TOP_DOWN
      ULONG EnableHandleExceptions : 1;   // FLG_ENABLE_HANDLE_EXCEPTIONS
      ULONG DisableProtDlls : 1;          // FLG_DISABLE_PROTDLLS
    } NtGlobalFlags;
  };

  LARGE_INTEGER CriticalSectionTimeout;
  SIZE_T HeapSegmentReserve;
  SIZE_T HeapSegmentCommit;
  SIZE_T HeapDeCommitTotalFreeThreshold;
  SIZE_T HeapDeCommitFreeBlockThreshold;

  ULONG NumberOfHeaps;
  ULONG MaximumNumberOfHeaps;
  PVOID *ProcessHeaps; // Array of heap pointers

  PGDI_HANDLE_ENTRY GdiSharedHandleTable;
  PVOID ProcessStarterHelper;
  ULONG GdiDCAttributeList; // GdiSetBatchLimit
  PRTL_CRITICAL_SECTION LoaderLock;

  ULONG OSMajorVersion;
  ULONG OSMinorVersion;
  USHORT OSBuildNumber;
  USHORT OSCSDVersion;
  ULONG OSPlatformId;

  ULONG ImageSubsystem; // PE subsystem
  ULONG ImageSubsystemMajorVersion;
  ULONG ImageSubsystemMinorVersion;

  KAFFINITY ActiveProcessAffinityMask;
  GDI_HANDLE_BUFFER GdiHandleBuffer; // GDI batch buffer

  PPS_POST_PROCESS_INIT_ROUTINE PostProcessInitRoutine;

  PRTL_BITMAP TlsExpansionBitmap;
  ULONG TlsExpansionBitmapBits[32]; // TLS_EXPANSION_SLOTS

  ULONG SessionId; // +0x2C0

  ULARGE_INTEGER AppCompatFlags; // KACF_* flags
  ULARGE_INTEGER AppCompatFlagsUser;
  PSHIM_PROCESS_CONTEXT pShimData; // AppCompat shim engine
  PVOID AppCompatInfo;             // PAPPCOMPAT_EXE_DATA
  UNICODE_STRING CSDVersion;       // OS CSD version string

  PVOID ActivationContextData; // PACTIVATION_CONTEXT_DATA
  PASSEMBLY_STORAGE_MAP ProcessAssemblyStorageMap;
  PVOID SystemDefaultActivationContextData; // PACTIVATION_CONTEXT_DATA
  PASSEMBLY_STORAGE_MAP SystemAssemblyStorageMap;

  SIZE_T MinimumStackCommit;

  // since 19H1 (previously FlsCallback to FlsHighIndex)
  PVOID SparePointers[2];
  PVOID PatchLoaderData;
  PVOID ChpeV2ProcessInfo; // CHPEV2_PROCESS_INFO

  ULONG AppModelFeatureState;
  ULONG SpareUlongs[2];

  USHORT ActiveCodePage;
  USHORT OemCodePage;
  USHORT UseCaseMapping;
  USHORT UnusedNlsField;

  PWER_PEB_HEADER_BLOCK WerRegistrationData;
  PVOID WerShipAssertPtr;

  union {
    PVOID pContextData; // SwitchBack compat (Win7-)
    PVOID EcCodeBitMap; // EC bitmap ARM64 (since WIN11)
  };

  PVOID ImageHeaderHash;

  union {
    ULONG TracingFlags;
    struct {
      ULONG HeapTracingEnabled : 1;      // ETW heap tracing
      ULONG CritSecTracingEnabled : 1;   // ETW lock tracing
      ULONG LibLoaderTracingEnabled : 1; // ETW loader tracing
      ULONG SpareTracingBits : 29;
    };
  };

  ULONGLONG CsrServerReadOnlySharedMemoryBase;
  PRTL_CRITICAL_SECTION TppWorkerpListLock; // Thread pool lock
  LIST_ENTRY TppWorkerpList;                // Thread pool worker list
  PVOID WaitOnAddressHashTable[128];        // RtlWaitOnAddress buckets
  PTELEMETRY_COVERAGE_HEADER TelemetryCoverageHeader; // since RS3

  ULONG CloudFileFlags; // ProjFs / Cloud Files (since RS4)
  ULONG CloudFileDiagFlags;
  CHAR PlaceholderCompatibilityMode;
  CHAR PlaceholderCompatibilityModeReserved[7];

  PLEAP_SECOND_DATA LeapSecondData; // since RS5

  union {
    ULONG LeapSecondFlags;
    struct {
      ULONG SixtySecondEnabled : 1; // Leap seconds enabled
      ULONG Reserved : 31;
    };
  };

  ULONG NtGlobalFlag2;
  ULONGLONG ExtendedFeatureDisableMask; // AVX disable mask (since WIN11)
} PEB, *PPEB;

#ifdef _WIN64
static_assert(FIELD_OFFSET(PEB, SessionId) == 0x2C0,
              "PEB::SessionId offset mismatch");
static_assert(sizeof(PEB) == 0x7d0, "PEB size mismatch (expected WIN11 24H2)");
#else
static_assert(FIELD_OFFSET(PEB, SessionId) == 0x1D4,
              "PEB::SessionId offset mismatch");
static_assert(sizeof(PEB) == 0x488, "PEB size mismatch (expected WIN11 24H2)");
#endif

static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS, Environment) ==
                  0x80,
              "RTL_USER_PROCESS_PARAMETERS::Environment offset mismatch");
static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS, ConsoleHandle) ==
                  0x10,
              "RTL_USER_PROCESS_PARAMETERS::ConsoleHandle offset mismatch");
static_assert(__builtin_offsetof(PEB, ProcessParameters) == 0x20,
              "PEB::ProcessParameters offset mismatch");
static_assert(__builtin_offsetof(PEB, ProcessHeap) == 0x30,
              "PEB::ProcessHeap offset mismatch");
static_assert(__builtin_offsetof(PEB, TlsBitmap) == 0x78,
              "PEB::TlsBitmap offset mismatch");

//===----------------------------------------------------------------------===//
// CLIENT_ID — process/thread identifier pair (TEB::ClientId)
//===----------------------------------------------------------------------===//
//
// Fields hold numeric IDs cast to HANDLE width — not real kernel handles.
// Defined here (not nt_types.h) because the TEB embeds it directly.
// nt_types.h has its own CLIENT_ID for use by nt_process.h callers;
// they are layout-compatible.

typedef struct _CLIENT_ID32 {
  ULONG UniqueProcess;
  ULONG UniqueThread;
} CLIENT_ID32, *PCLIENT_ID32;

typedef struct _CLIENT_ID64 {
  ULONGLONG UniqueProcess;
  ULONGLONG UniqueThread;
} CLIENT_ID64, *PCLIENT_ID64;

//===----------------------------------------------------------------------===//
// LDR_RESLOADER_RET — TEB::ResourceRetValue (LdrFindResource result)
//===----------------------------------------------------------------------===//

typedef struct _LDR_RESLOADER_RET {
  PVOID Module;
  PVOID DataEntry;
  PVOID TargetModule;
} LDR_RESLOADER_RET, *PLDR_RESLOADER_RET;

//===----------------------------------------------------------------------===//
// GDI_TEB_BATCH — GDI batch accumulator (TEB::GdiTebBatch)
//===----------------------------------------------------------------------===//

#define GDI_BATCH_BUFFER_SIZE 310

typedef struct _GDI_TEB_BATCH {
  ULONG Offset;
  ULONG_PTR HDC;
  ULONG Buffer[GDI_BATCH_BUFFER_SIZE];
} GDI_TEB_BATCH, *PGDI_TEB_BATCH;

//===----------------------------------------------------------------------===//
// MSG — Win32 window message (used in CLIENTINFO::msgDbcsCB)
//===----------------------------------------------------------------------===//

typedef struct tagMSG {
  HWND hwnd;
  UINT message;
  WPARAM wParam;
  LPARAM lParam;
  DWORD time;
  POINT pt;
  DWORD lPrivate;
} MSG, *PMSG, *NPMSG, *LPMSG;

//===----------------------------------------------------------------------===//
// CALLBACKWND — user32 callback window state
//===----------------------------------------------------------------------===//

typedef struct _CALLBACKWND {
  HWND hwnd;
  ULONG_PTR pwnd;
  PACTIVATION_CONTEXT ActCtx;
} CALLBACKWND, *PCALLBACKWND;

//===----------------------------------------------------------------------===//
// CLIENTINFO — Win32 per-thread client state (TEB::Win32ClientInfo)
//===----------------------------------------------------------------------===//

typedef struct tagCLIENTINFO {
  ULONG_PTR CI_flags;
  ULONG_PTR Spins;
  ULONG ExpWinVer;
  ULONG CompatFlags;
  ULONG CompatFlags2;
  ULONG TIFlags;
  struct tagDESKTOPINFO *DeskInfo; // win32k opaque
  PVOID DesktopBase;               // ClientDelta before RS2
  HHOOK hkCurrent;
  ULONG Hooks;
  CALLBACKWND CallbackWnd;
  ULONG HookCurrent;
  LONG InDDEMLCallback;
  struct tagCLIENTTHREADINFO *ClientThreadInfo; // win32k opaque
  ULONG_PTR HookData;
  ULONG KeyCache;
  UCHAR KeyState[8];
  ULONG AsyncKeyCache;
  UCHAR AsyncKeyState[8];
  UCHAR AsyncKeyStateRecentDown[8];
  HANDLE hKL; // HKL keyboard layout
  USHORT CodePage;
  UCHAR DbcsCFOld[2];
  UCHAR DbcsCFNew[2];
  MSG msgDbcsCB;
  PULONG RegisteredClasses;
  HANDLE mmcssHandle;
  ULONG_PTR CI_exflags;
  DPICONTEXTINFO dci;
} CLIENTINFO, *PCLIENTINFO;

// PROCESSOR_NUMBER is now in <sys/ntabi.h>.

//===----------------------------------------------------------------------===//
// TEB_ACTIVE_FRAME — frame-based context chain (TEB::ActiveFrame)
//===----------------------------------------------------------------------===//

#define TEB_ACTIVE_FRAME_CONTEXT_FLAG_EXTENDED 0x00000001
#define TEB_ACTIVE_FRAME_FLAG_EXTENDED 0x00000001

typedef struct _TEB_ACTIVE_FRAME_CONTEXT {
  ULONG Flags;
  PCSTR FrameName;
} TEB_ACTIVE_FRAME_CONTEXT, *PTEB_ACTIVE_FRAME_CONTEXT;

typedef struct _TEB_ACTIVE_FRAME_CONTEXT_EX {
  TEB_ACTIVE_FRAME_CONTEXT BasicContext;
  PCSTR SourceLocation;
} TEB_ACTIVE_FRAME_CONTEXT_EX, *PTEB_ACTIVE_FRAME_CONTEXT_EX;

typedef struct _TEB_ACTIVE_FRAME {
  ULONG Flags;
  struct _TEB_ACTIVE_FRAME *Previous;
  PTEB_ACTIVE_FRAME_CONTEXT Context;
} TEB_ACTIVE_FRAME, *PTEB_ACTIVE_FRAME;

typedef struct _TEB_ACTIVE_FRAME_EX {
  TEB_ACTIVE_FRAME BasicFrame;
  PVOID ExtensionIdentifier;
} TEB_ACTIVE_FRAME_EX, *PTEB_ACTIVE_FRAME_EX;

//===----------------------------------------------------------------------===//
// TEB — Thread Environment Block
//===----------------------------------------------------------------------===//
//
// Offsets (x64):
//   +0x000  NtTib.Self (gs:0x30)
//   +0x008  StackBase
//   +0x010  StackLimit
//   +0x020  FiberData
//   +0x030  Self
//   +0x038  EnvironmentPointer
//   +0x040  ClientId.UniqueProcess (PID)
//   +0x048  ClientId.UniqueThread  (TID)
//   +0x058  ThreadLocalStoragePointer
//   +0x060  ProcessEnvironmentBlock (PEB pointer)
//   +0x068  LastErrorValue
//   +0x100  WOW32Reserved
//   +0x1478 DeallocationStack
//   +0x1480 TlsSlots
//   +0x1744 CurrentIdealProcessor
//   +0x1780 TlsExpansionSlots
//   +0x17C8 FlsData
//   +0x1850 SchedulerSharedDataSlot (since 24H2)
//   +0x1860 PrimaryGroupAffinity

#define STATIC_UNICODE_BUFFER_LENGTH 261
#define WIN32_CLIENT_INFO_LENGTH 62
#define TLS_MINIMUM_AVAILABLE 0x40
#define TLS_EXPANSION_SLOTS 0x400

// Well-known TEB offsets (x64)
#define TeCmTeb 0x0
#define TeStackBase 0x8
#define TeStackLimit 0x10
#define TeFiberData 0x20
#define TeSelf 0x30
#define TeEnvironmentPointer 0x38
#define TeClientId 0x40
#define TeActiveRpcHandle 0x50
#define TeThreadLocalStoragePointer 0x58
#define TePeb 0x60
#define TeLastErrorValue 0x68
#define TeCountOfOwnedCriticalSections 0x6c
#define TeCsrClientThread 0x70
#define TeWOW32Reserved 0x100
#define TeSoftFpcr 0x10c
#define TeExceptionCode 0x2c0
#define TeActivationContextStackPointer 0x2c8
#define TeInstrumentationCallbackSp 0x2d0
#define TeInstrumentationCallbackPreviousPc 0x2d8
#define TeInstrumentationCallbackPreviousSp 0x2e0
#define TeUnalignedLoadStoreExceptions 0x2ed
#define TeGdiClientPID 0x7f0
#define TeGdiClientTID 0x7f4
#define TeGdiThreadLocalInfo 0x7f8
#define TeglDispatchTable 0x9f0
#define TeglReserved1 0x1138
#define TeglReserved2 0x1220
#define TeglSectionInfo 0x1228
#define TeglSection 0x1230
#define TeglTable 0x1238
#define TeglCurrentRC 0x1240
#define TeglContext 0x1248
#define TeDeallocationStack 0x1478
#define TeTlsSlots 0x1480
#define TeTlsExpansionSlots 0x1780
#define TeVdm 0x1690
#define TeInstrumentation 0x16b8
#define TeGdiBatchCount 0x1740
#define TeCurrentIdealProcessor 0x1744
#define TeGuaranteedStackBytes 0x1748
#define TeFlsData 0x17c8
#define TeChpeV2CpuAreaInfo 0x1788
#define TePrimaryGroupAffinity 0x1860
#define ThreadEnvironmentBlockLength 0x1878
#define CmThreadEnvironmentBlockOffset 0x2000

typedef struct _TEB {
  NT_TIB NtTib; // +0x000 — SEH chain, stack bounds, self-pointer
  PVOID EnvironmentPointer;
  CLIENT_ID ClientId; // +0x040 — PID/TID pair
  PVOID ActiveRpcHandle;
  PVOID ThreadLocalStoragePointer; // __declspec(thread) array
  PPEB ProcessEnvironmentBlock;    // +0x060 — PEB pointer
  ULONG LastErrorValue;            // +0x068
  ULONG CountOfOwnedCriticalSections;
  PVOID CsrClientThread;
  PVOID Win32ThreadInfo; // win32k thread info

  ULONG User32Reserved[26]; // user32.dll
  ULONG UserReserved[5];    // winsrv.dll
  PVOID WOW32Reserved;      // +0x100

  LCID CurrentLocale; // Kernel32!GetThreadLocale
  ULONG FpSoftwareStatusRegister;
  PVOID ReservedForDebuggerInstrumentation[16];

#ifdef _WIN64
  PVOID SystemReserved1[25]; // Float-point emulation
  PVOID HeapFlsData;         // Per-thread fiber local storage
  ULONG_PTR RngState[4];
#else
  PVOID SystemReserved1[26];
#endif

  CHAR PlaceholderCompatibilityMode; // ProjFs / Cloud Files
  BOOLEAN PlaceholderHydrationAlwaysExplicit;
  CHAR PlaceholderReserved[10];
  ULONG ProxiedProcessId; // COM server on-behalf-of PID

  ACTIVATION_CONTEXT_STACK ActivationStack; // Embedded, sizeof=0x28
  UCHAR WorkingOnBehalfTicket[8];
  NTSTATUS ExceptionCode;                                  // +0x2C0
  PACTIVATION_CONTEXT_STACK ActivationContextStackPointer; // +0x2C8

  ULONG_PTR InstrumentationCallbackSp;         // +0x2D0
  ULONG_PTR InstrumentationCallbackPreviousPc; // +0x2D8
  ULONG_PTR InstrumentationCallbackPreviousSp; // +0x2E0

#ifdef _WIN64
  ULONG TxFsContext; // TxF miniversion ID
#endif

  BOOLEAN InstrumentationCallbackDisabled;

#ifdef _WIN64
  BOOLEAN UnalignedLoadStoreExceptions; // +0x2ED
#else
  UCHAR SpareBytes[23];
  ULONG TxFsContext;
#endif

  // GDI batch state
  GDI_TEB_BATCH GdiTebBatch;
  CLIENT_ID RealClientId;
  HANDLE GdiCachedProcessHandle;
  ULONG GdiClientPID;
  ULONG GdiClientTID;
  PVOID GdiThreadLocalInfo;

  // User32 per-thread client info (user mode only)
  union {
    CLIENTINFO Win32ClientInfo;
    ULONG_PTR Win32ClientInfoArea[WIN32_CLIENT_INFO_LENGTH];
  };

  // OpenGL dispatch (glDispatchTable)
  PVOID glDispatchTable[233];
  ULONG_PTR glReserved1[29];
  PVOID glReserved2;
  PVOID glSectionInfo;
  PVOID glSection;
  PVOID glTable;
  PVOID glCurrentRC;
  PVOID glContext;

  NTSTATUS LastStatusValue;

  // Static Unicode string buffer for application use
  UNICODE_STRING StaticUnicodeString;
  WCHAR StaticUnicodeBuffer[STATIC_UNICODE_BUFFER_LENGTH];

  PVOID DeallocationStack;               // +0x1478 — stack base
  PVOID TlsSlots[TLS_MINIMUM_AVAILABLE]; // +0x1480 — TLS data (TlsGetValue)
  LIST_ENTRY TlsLinks;
  PVOID Vdm; // NTVDM

  // RPC reserved (pointer XOR'd with RPC_THREAD_POINTER_KEY)
  PVOID ReservedForNtRpc;

  PVOID DbgSsReserved[2]; // DebugActiveProcess
  ULONG HardErrorMode;    // GetThreadErrorMode

#ifdef _WIN64
  PVOID Instrumentation[11];
#else
  PVOID Instrumentation[9];
#endif

  GUID ActivityId;
  PVOID SubProcessTag; // svchost service ID
  PVOID PerflibData;
  PVOID EtwTraceData;
  HANDLE WinSockData;  // Socket handle during blocking WSA op
  ULONG GdiBatchCount; // +0x1740 — GdiSetBatchLimit

  // Preferred processor (SetThreadIdealProcessorEx)
  union {
    PROCESSOR_NUMBER CurrentIdealProcessor; // +0x1744
    ULONG IdealProcessorValue;
    struct {
      UCHAR ReservedPad0;
      UCHAR ReservedPad1;
      UCHAR ReservedPad2;
      UCHAR IdealProcessor;
    };
  };

  ULONG GuaranteedStackBytes; // +0x1748 — SetThreadStackGuarantee
  PVOID ReservedForPerf;
  PSOleTlsData ReservedForOle; // OLE per-thread state
  ULONG WaitingOnLoaderLock;
  PVOID SavedPriorityState;
  ULONG_PTR ReservedForCodeCoverage;
  PVOID ThreadPoolData;

  PVOID *TlsExpansionSlots; // +0x1780

#ifdef _WIN64
  PVOID ChpeV2CpuAreaInfo; // +0x1788 — CHPEV2_CPUAREA_INFO
  PVOID Unused;            // previously BStoreLimit
#endif

  ULONG MuiGeneration;
  ULONG IsImpersonating;
  PVOID NlsCache;  // NLS cache
  PVOID pShimData; // AppCompat shim engine
  ULONG HeapData;
  HANDLE CurrentTransactionHandle; // KTM transaction
  PTEB_ACTIVE_FRAME ActiveFrame;
  PVOID FlsData; // +0x17C8 — FLS (RtlProcessFlsData)

  // MUI preferred language lists
  PVOID PreferredLanguages;  // GetThreadPreferredUILanguages
  PVOID UserPrefLanguages;   // GetUserPreferredUILanguages
  PVOID MergedPrefLanguages; // MUI_MERGE_USER_FALLBACK
  ULONG MuiImpersonation;

  union {
    USHORT CrossTebFlags;
    USHORT SpareCrossTebBits : 16;
  };

  union {
    USHORT SameTebFlags;
    struct {
      USHORT SafeThunkCall : 1;
      USHORT InDebugPrint : 1;     // In debug print routine
      USHORT HasFiberData : 1;     // Has fiber-local storage
      USHORT SkipThreadAttach : 1; // Suppress DLL_THREAD_ATTACH
      USHORT WerInShipAssertCode : 1;
      USHORT RanProcessInit : 1;   // Ran process init code
      USHORT ClonedThread : 1;     // Clone of another thread
      USHORT SuppressDebugMsg : 1; // Suppress LOAD_DLL_DEBUG_INFO
      USHORT DisableUserStackWalk : 1;
      USHORT RtlExceptionAttached : 1;
      USHORT InitialThread : 1; // Initial thread of process
      USHORT SessionAware : 1;
      USHORT LoadOwner : 1; // Owns loader lock
      USHORT LoaderWorker : 1;
      USHORT SkipLoaderInit : 1;
      USHORT SkipFileAPIBrokering : 1;
    };
  };

  // KTM transaction scope callbacks
  PVOID TxnScopeEnterCallback;
  PVOID TxnScopeExitCallback;
  PVOID TxnScopeContext;

  ULONG LockCount;   // Critical section lock count
  LONG WowTebOffset; // WOW64 TEB offset

  // LdrFindResource result (module + data entry)
  PLDR_RESLOADER_RET ResourceRetValue;

  PVOID ReservedForWdf;      // Windows Driver Framework
  ULONGLONG ReservedForCrt;  // Microsoft CRT
  GUID EffectiveContainerId; // HCS container ID

  ULONGLONG LastSleepCounter; // since Win11 — Kernel32!Sleep
  ULONG SpinCallCount;
  ULONGLONG ExtendedFeatureDisableMask; // AVX disable mask

  PVOID SchedulerSharedDataSlot; // +0x1850 — since 24H2
  PVOID HeapWalkContext;
  GROUP_AFFINITY PrimaryGroupAffinity; // +0x1860
  ULONG Rcu[2];                        // RCU synchronization context
} TEB, *PTEB;

#ifdef _WIN64
static_assert(FIELD_OFFSET(TEB, SchedulerSharedDataSlot) == 0x1850,
              "TEB::SchedulerSharedDataSlot offset mismatch");
static_assert(sizeof(TEB) == 0x1878, "TEB size mismatch (expected WIN11 24H2)");
#else
static_assert(FIELD_OFFSET(TEB, SchedulerSharedDataSlot) == 0x1018,
              "TEB::SchedulerSharedDataSlot offset mismatch");
static_assert(sizeof(TEB) == 0x1038, "TEB size mismatch (expected WIN11 24H2)");
#endif

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PEB_H
