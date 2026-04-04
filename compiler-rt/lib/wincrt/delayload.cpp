//===-- delayload.cpp - Delay load helper ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Delay load helper compatible with MSVC's delayimp.h.
// See: https://learn.microsoft.com/en-us/cpp/build/reference/understanding-the-helper-function
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "internal.h"

// Matches IMAGE_DELAYLOAD_DESCRIPTOR in winnt.h.
struct WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR {
  union {
    DWORD AllAttributes;
    struct {
      DWORD RvaBased : 1;
      DWORD ReservedAttributes : 31;
    };
  } Attributes;
  DWORD DllNameRVA;
  DWORD ModuleHandleRVA;
  DWORD ImportAddressTableRVA;
  DWORD ImportNameTableRVA;
  DWORD BoundImportAddressTableRVA;
  DWORD UnloadInformationTableRVA;
  DWORD TimeDateStamp;
};

struct WINCRT_IMAGE_THUNK_DATA {
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
  unsigned __int64 u1;
#else
  DWORD u1;
#endif
};

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
constexpr unsigned __int64 kImageOrdinalFlag = 0x8000000000000000ULL;
#else
constexpr DWORD kImageOrdinalFlag = 0x80000000UL;
#endif

// Native delay load API (delayloadhandler.h, Win8+).
struct WINCRT_DELAYLOAD_PROC_DESCRIPTOR {
  DWORD ImportDescribedByName;
  union {
    LPCSTR Name;
    DWORD Ordinal;
  } Description;
};

struct WINCRT_DELAYLOAD_INFO {
  DWORD Size;
  const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR *DelayloadDescriptor;
  WINCRT_IMAGE_THUNK_DATA *ThunkAddress;
  LPCSTR TargetDllName;
  WINCRT_DELAYLOAD_PROC_DESCRIPTOR TargetApiDescriptor;
  void *TargetModuleBase;
  void *Unused;
  DWORD LastError;
};

constexpr DWORD kDelayloadGpaFailure = 4;

using DelayloadFailureCallback = void *(__stdcall *)(DWORD, WINCRT_DELAYLOAD_INFO *);

extern "C" {
__declspec(dllimport) void *__stdcall ResolveDelayLoadedAPI(
    void *, const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR *,
    DelayloadFailureCallback, void *, WINCRT_IMAGE_THUNK_DATA *, DWORD);
__declspec(dllimport) NTSTATUS __stdcall ResolveDelayLoadsFromDll(
    void *, LPCSTR, DWORD);
}

// User hook types (delayimp.h compatible).
struct DelayLoadProc {
  BOOL fImportByName;
  union {
    LPCSTR szProcName;
    DWORD dwOrdinal;
  };
};

struct DelayLoadInfo {
  DWORD cb;
  const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR *pidd;
  void **ppfn;
  LPCSTR szDll;
  DelayLoadProc dlp;
  HMODULE hmodCur;
  void *pfnCur;
  DWORD dwLastError;
};

using PfnDliHook = void *(__stdcall *)(unsigned, DelayLoadInfo *);

enum {
  dliStartProcessing = 0,
  dliNoteStartProcessing = 0,
  dliNotePreLoadLibrary = 1,
  dliNotePreGetProcAddress = 2,
  dliFailLoadLib = 3,
  dliFailGetProc = 4,
  dliNoteEndProcessing = 5,
};

// Hooks are const (.rdata) to prevent hijacking via memory corruption.
// Define DELAYIMP_INSECURE_WRITABLE_HOOKS for runtime-settable hooks.
extern "C" const PfnDliHook __pfnDliNotifyHook2;
extern "C" const PfnDliHook __pfnDliFailureHook2;

#pragma comment(linker, "/alternatename:" WINCRT_SYM_PREFIX "__pfnDliNotifyHook2=" \
                                          WINCRT_SYM_PREFIX "__wincrt_dli_notify_hook_default")
#pragma comment(linker, "/alternatename:" WINCRT_SYM_PREFIX "__pfnDliFailureHook2=" \
                                          WINCRT_SYM_PREFIX "__wincrt_dli_failure_hook_default")

extern "C" {
const PfnDliHook __wincrt_dli_notify_hook_default = nullptr;
const PfnDliHook __wincrt_dli_failure_hook_default = nullptr;
}

extern "C" const char __ImageBase;

// Minimal PE structures for delay import directory traversal.
struct WINCRT_IMAGE_DOS_HEADER {
  WORD e_magic;
  WORD e_cblp;
  WORD e_cp;
  WORD e_crlc;
  WORD e_cparhdr;
  WORD e_minalloc;
  WORD e_maxalloc;
  WORD e_ss;
  WORD e_sp;
  WORD e_csum;
  WORD e_ip;
  WORD e_cs;
  WORD e_lfarlc;
  WORD e_ovno;
  WORD e_res[4];
  WORD e_oemid;
  WORD e_oeminfo;
  WORD e_res2[10];
  LONG e_lfanew;
};

struct WINCRT_IMAGE_DATA_DIRECTORY {
  DWORD VirtualAddress;
  DWORD Size;
};

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
struct WINCRT_IMAGE_OPTIONAL_HEADER {
  WORD Magic;
  BYTE MajorLinkerVersion;
  BYTE MinorLinkerVersion;
  DWORD SizeOfCode;
  DWORD SizeOfInitializedData;
  DWORD SizeOfUninitializedData;
  DWORD AddressOfEntryPoint;
  DWORD BaseOfCode;
  DWORD64 ImageBase;
  DWORD SectionAlignment;
  DWORD FileAlignment;
  WORD MajorOperatingSystemVersion;
  WORD MinorOperatingSystemVersion;
  WORD MajorImageVersion;
  WORD MinorImageVersion;
  WORD MajorSubsystemVersion;
  WORD MinorSubsystemVersion;
  DWORD Win32VersionValue;
  DWORD SizeOfImage;
  DWORD SizeOfHeaders;
  DWORD CheckSum;
  WORD Subsystem;
  WORD DllCharacteristics;
  DWORD64 SizeOfStackReserve;
  DWORD64 SizeOfStackCommit;
  DWORD64 SizeOfHeapReserve;
  DWORD64 SizeOfHeapCommit;
  DWORD LoaderFlags;
  DWORD NumberOfRvaAndSizes;
  WINCRT_IMAGE_DATA_DIRECTORY DataDirectory[16];
};
#else
struct WINCRT_IMAGE_OPTIONAL_HEADER {
  WORD Magic;
  BYTE MajorLinkerVersion;
  BYTE MinorLinkerVersion;
  DWORD SizeOfCode;
  DWORD SizeOfInitializedData;
  DWORD SizeOfUninitializedData;
  DWORD AddressOfEntryPoint;
  DWORD BaseOfCode;
  DWORD BaseOfData;
  DWORD ImageBase;
  DWORD SectionAlignment;
  DWORD FileAlignment;
  WORD MajorOperatingSystemVersion;
  WORD MinorOperatingSystemVersion;
  WORD MajorImageVersion;
  WORD MinorImageVersion;
  WORD MajorSubsystemVersion;
  WORD MinorSubsystemVersion;
  DWORD Win32VersionValue;
  DWORD SizeOfImage;
  DWORD SizeOfHeaders;
  DWORD CheckSum;
  WORD Subsystem;
  WORD DllCharacteristics;
  DWORD SizeOfStackReserve;
  DWORD SizeOfStackCommit;
  DWORD SizeOfHeapReserve;
  DWORD SizeOfHeapCommit;
  DWORD LoaderFlags;
  DWORD NumberOfRvaAndSizes;
  WINCRT_IMAGE_DATA_DIRECTORY DataDirectory[16];
};
#endif

struct WINCRT_IMAGE_FILE_HEADER {
  WORD Machine;
  WORD NumberOfSections;
  DWORD TimeDateStamp;
  DWORD PointerToSymbolTable;
  DWORD NumberOfSymbols;
  WORD SizeOfOptionalHeader;
  WORD Characteristics;
};

struct WINCRT_IMAGE_NT_HEADERS {
  DWORD Signature;
  WINCRT_IMAGE_FILE_HEADER FileHeader;
  WINCRT_IMAGE_OPTIONAL_HEADER OptionalHeader;
};

constexpr DWORD kImageDirectoryEntryDelayImport = 13;

extern "C" {
__declspec(dllimport) int __stdcall lstrcmpiA(LPCSTR, LPCSTR);
}

namespace {

template <typename T>
T *rvaToPtr(DWORD rva) {
  if (rva == 0)
    return nullptr;
  return reinterpret_cast<T *>(const_cast<char *>(&__ImageBase) + rva);
}

bool strEqualsIgnoreCase(LPCSTR a, LPCSTR b) {
  return lstrcmpiA(a, b) == 0;
}

const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR *getDelayImportDirectory() {
  auto *dosHeader = reinterpret_cast<const WINCRT_IMAGE_DOS_HEADER *>(&__ImageBase);
  auto *ntHeaders = reinterpret_cast<const WINCRT_IMAGE_NT_HEADERS *>(
      &__ImageBase + dosHeader->e_lfanew);

  if (ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= kImageDirectoryEntryDelayImport)
    return nullptr;

  const WINCRT_IMAGE_DATA_DIRECTORY &dir =
      ntHeaders->OptionalHeader.DataDirectory[kImageDirectoryEntryDelayImport];
  if (dir.VirtualAddress == 0 || dir.Size == 0)
    return nullptr;

  return rvaToPtr<const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR>(dir.VirtualAddress);
}

size_t countIatEntries(const WINCRT_IMAGE_THUNK_DATA *iat) {
  size_t count = 0;
  while (iat[count].u1 != 0)
    ++count;
  return count;
}

// CFG may mark delay IAT read-only; temporarily make writable for restoration.
void overlayIat(WINCRT_IMAGE_THUNK_DATA *dst, const WINCRT_IMAGE_THUNK_DATA *src,
                size_t count) {
  if (count == 0)
    return;

  size_t size = count * sizeof(WINCRT_IMAGE_THUNK_DATA);
  DWORD oldProtect = 0;

  VirtualProtect(dst, size, PAGE_READWRITE, &oldProtect);

  for (size_t i = 0; i < count; ++i)
    dst[i].u1 = src[i].u1;

  if (oldProtect != 0 && oldProtect != PAGE_READWRITE) {
    DWORD unused;
    VirtualProtect(dst, size, oldProtect, &unused);
  }
}

void *__stdcall failureHookAdapter(DWORD reason, WINCRT_DELAYLOAD_INFO *info) {
  if (!__pfnDliFailureHook2)
    return nullptr;

  DelayLoadInfo dli = {};
  dli.cb = sizeof(dli);
  dli.pidd = info->DelayloadDescriptor;
  dli.ppfn = reinterpret_cast<void **>(info->ThunkAddress);
  dli.szDll = info->TargetDllName;
  dli.dlp.fImportByName = info->TargetApiDescriptor.ImportDescribedByName;
  if (dli.dlp.fImportByName)
    dli.dlp.szProcName = info->TargetApiDescriptor.Description.Name;
  else
    dli.dlp.dwOrdinal = info->TargetApiDescriptor.Description.Ordinal;
  dli.hmodCur = static_cast<HMODULE>(info->TargetModuleBase);
  dli.dwLastError = info->LastError;

  unsigned dliReason =
      (reason == kDelayloadGpaFailure) ? dliFailGetProc : dliFailLoadLib;
  return __pfnDliFailureHook2(dliReason, &dli);
}

} // namespace

extern "C" {

// Fast path uses native API when no notify hooks; slow path for full hook support.
void *__stdcall __delayLoadHelper2(
    const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR *pidd, void **ppfnIATEntry) {

  if (!__pfnDliNotifyHook2) {
    return ResolveDelayLoadedAPI(
        const_cast<char *>(&__ImageBase), pidd,
        __pfnDliFailureHook2 ? failureHookAdapter : nullptr, nullptr,
        reinterpret_cast<WINCRT_IMAGE_THUNK_DATA *>(ppfnIATEntry), 0);
  }

  // Full manual resolution with notify hooks (rare path).
  if (!pidd->Attributes.RvaBased) {
    OutputDebugStringA("WINCRT FATAL: delay load v1 format not supported\n");
    return nullptr;
  }

  LPCSTR szDll = rvaToPtr<const char>(pidd->DllNameRVA);
  HMODULE *phmod = rvaToPtr<HMODULE>(pidd->ModuleHandleRVA);

  auto *pIAT = rvaToPtr<WINCRT_IMAGE_THUNK_DATA>(pidd->ImportAddressTableRVA);
  auto *pINT = rvaToPtr<WINCRT_IMAGE_THUNK_DATA>(pidd->ImportNameTableRVA);
  auto *pIATEntry = reinterpret_cast<WINCRT_IMAGE_THUNK_DATA *>(ppfnIATEntry);

  size_t iIAT = pIATEntry - pIAT;

  DelayLoadInfo dli = {};
  dli.cb = sizeof(dli);
  dli.pidd = pidd;
  dli.ppfn = reinterpret_cast<void **>(&pIATEntry->u1);
  dli.szDll = szDll;

  WINCRT_IMAGE_THUNK_DATA *pINTEntry = &pINT[iIAT];
  if (pINTEntry->u1 & kImageOrdinalFlag) {
    dli.dlp.fImportByName = 0;
    dli.dlp.dwOrdinal = static_cast<DWORD>(pINTEntry->u1 & 0xFFFF);
  } else {
    dli.dlp.fImportByName = 1;
    // Skip 2-byte hint to get name string.
    dli.dlp.szProcName = rvaToPtr<const char>(static_cast<DWORD>(pINTEntry->u1)) + 2;
  }

  // dliStartProcessing: hook can bypass entire resolution.
  if (__pfnDliNotifyHook2) {
    void *result = __pfnDliNotifyHook2(dliStartProcessing, &dli);
    if (result) {
      pIATEntry->u1 = reinterpret_cast<decltype(pIATEntry->u1)>(result);
      return result;
    }
  }

  HMODULE hmod = phmod ? *phmod : nullptr;
  if (!hmod) {
    if (__pfnDliNotifyHook2)
      hmod = static_cast<HMODULE>(__pfnDliNotifyHook2(dliNotePreLoadLibrary, &dli));

    if (!hmod)
      hmod = LoadLibraryA(szDll);

    if (!hmod) {
      dli.dwLastError = GetLastError();
      if (__pfnDliFailureHook2)
        hmod = static_cast<HMODULE>(__pfnDliFailureHook2(dliFailLoadLib, &dli));

      if (!hmod) {
        SetLastError(dli.dwLastError);
        return nullptr;
      }
    }

    // Thread-safe module handle storage.
    if (phmod) {
      HMODULE expected = nullptr;
      if (!__atomic_compare_exchange_n(phmod, &expected, hmod, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // Another thread loaded first; use their handle.
        hmod = expected;
      }
    }
  }

  dli.hmodCur = hmod;

  void *pfn = nullptr;
  if (__pfnDliNotifyHook2)
    pfn = __pfnDliNotifyHook2(dliNotePreGetProcAddress, &dli);

  if (!pfn) {
    if (dli.dlp.fImportByName)
      pfn = reinterpret_cast<void *>(GetProcAddress(hmod, dli.dlp.szProcName));
    else
      pfn = reinterpret_cast<void *>(GetProcAddress(hmod, reinterpret_cast<LPCSTR>(
          static_cast<uintptr_t>(dli.dlp.dwOrdinal))));
  }

  if (!pfn) {
    dli.dwLastError = GetLastError();
    if (__pfnDliFailureHook2)
      pfn = __pfnDliFailureHook2(dliFailGetProc, &dli);

    if (!pfn) {
      SetLastError(dli.dwLastError);
      return nullptr;
    }
  }

  dli.pfnCur = pfn;
  pIATEntry->u1 = reinterpret_cast<decltype(pIATEntry->u1)>(pfn);

  if (__pfnDliNotifyHook2)
    __pfnDliNotifyHook2(dliNoteEndProcessing, &dli);

  return pfn;
}

// Pre-load all delay imports. Bypasses notify hooks (matches MSVC behavior).
HRESULT __stdcall __HrLoadAllImportsForDll(LPCSTR szDll) {
  // STATUS_DLL_NOT_FOUND is defined in winnt.h as 0xC0000135L.
  constexpr HRESULT E_MOD_NOT_FOUND = 0x8007007EL;

  NTSTATUS status =
      ResolveDelayLoadsFromDll(const_cast<char *>(&__ImageBase), szDll, 0);
  return (static_cast<DWORD>(status) == STATUS_DLL_NOT_FOUND) ? E_MOD_NOT_FOUND : 0;
}

// Unload delay-loaded DLL. Requires /delay:unload at link time.
BOOL __stdcall __FUnloadDelayLoadedDLL2(LPCSTR szDll) {
  const WINCRT_IMAGE_DELAYLOAD_DESCRIPTOR *pidd = getDelayImportDirectory();
  if (!pidd)
    return 0;

  for (; pidd->DllNameRVA != 0; ++pidd) {
    if (!pidd->Attributes.RvaBased)
      continue;

    LPCSTR dllName = rvaToPtr<const char>(pidd->DllNameRVA);
    if (!strEqualsIgnoreCase(dllName, szDll))
      continue;

    if (pidd->UnloadInformationTableRVA == 0)
      return 0;

    HMODULE *phmod = rvaToPtr<HMODULE>(pidd->ModuleHandleRVA);
    if (!phmod)
      return 0;

    HMODULE hmod = __atomic_exchange_n(phmod, nullptr, __ATOMIC_ACQ_REL);
    if (!hmod)
      return 0;

    FreeLibrary(hmod);

    WINCRT_IMAGE_THUNK_DATA *iat = rvaToPtr<WINCRT_IMAGE_THUNK_DATA>(
        pidd->ImportAddressTableRVA);
    const WINCRT_IMAGE_THUNK_DATA *unloadIat = rvaToPtr<const WINCRT_IMAGE_THUNK_DATA>(
        pidd->UnloadInformationTableRVA);

    if (iat && unloadIat) {
      size_t count = countIatEntries(unloadIat);
      overlayIat(iat, unloadIat, count);
    }

    return 1;
  }

  return 0;
}

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
