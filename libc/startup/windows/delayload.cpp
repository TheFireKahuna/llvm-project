//===-- delayload.cpp - Delay load helper for PE/COFF ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides __delayLoadHelper2, called by linker-generated thunks when a
// delay-loaded DLL import is first invoked. Resolves imports through the
// NT loader (LdrLoadDll / LdrGetProcedureAddress / LdrUnloadDll), with
// delayimp-compatible notify/failure hook behavior.
//
// Supports __pfnDliNotifyHook2 / __pfnDliFailureHook2 for MSVC delayimp.h
// compatibility via /alternatename fallback to null defaults.
//
//===----------------------------------------------------------------------===//

#include "delayload_types.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string.h"
#include "src/__support/ctype_utils.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

extern "C" const char __ImageBase;

//===----------------------------------------------------------------------===//
// User hook interface (delayimp.h compatible)
//===----------------------------------------------------------------------===//

struct DelayLoadProc {
  int fImportByName;
  union {
    LPCSTR szProcName;
    DWORD dwOrdinal;
  };
};

struct DelayLoadInfo {
  DWORD cb;
  const IMAGE_DELAYLOAD_DESCRIPTOR *pidd;
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
  dliNotePreLoadLibrary = 1,
  dliNotePreGetProcAddress = 2,
  dliFailLoadLib = 3,
  dliFailGetProc = 4,
  dliNoteEndProcessing = 5,
};

// Hook pointers — const (.rdata) by default to prevent hijacking.
// User code overrides via defining the symbol; /alternatename provides null.
extern "C" const PfnDliHook __pfnDliNotifyHook2;
extern "C" const PfnDliHook __pfnDliFailureHook2;

extern "C" {
extern __declspec(selectany) const PfnDliHook
    __wincrt_dli_notify_hook_default = nullptr;
extern __declspec(selectany) const PfnDliHook
    __wincrt_dli_failure_hook_default = nullptr;
}

#if defined(__i386__)
#define SYM_PREFIX "_"
#else
#define SYM_PREFIX ""
#endif

#pragma comment(linker, "/alternatename:" SYM_PREFIX                           \
                        "__pfnDliNotifyHook2=" SYM_PREFIX                       \
                        "__wincrt_dli_notify_hook_default")
#pragma comment(linker, "/alternatename:" SYM_PREFIX                           \
                        "__pfnDliFailureHook2=" SYM_PREFIX                      \
                        "__wincrt_dli_failure_hook_default")

//===----------------------------------------------------------------------===//
// Ordinal flag
//===----------------------------------------------------------------------===//

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
static constexpr unsigned long long kImageOrdinalFlag = 0x8000000000000000ULL;
#else
static constexpr DWORD kImageOrdinalFlag = 0x80000000UL;
#endif

namespace {

constexpr int kDelayloadDllNameMax = 261;

void set_last_error_from_status(NTSTATUS status) {
  ::RtlSetLastWin32Error(::RtlNtStatusToDosError(status));
}

HMODULE load_library_nt(LPCSTR dll_name) {
  WCHAR wide_name[kDelayloadDllNameMax];
  if (LIBC_NAMESPACE::windows::utf8_to_wide(dll_name, wide_name,
                                            kDelayloadDllNameMax) <= 0) {
    ::RtlSetLastWin32Error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }

  UNICODE_STRING dll_name_us;
  ::RtlInitUnicodeString(&dll_name_us, wide_name);

  PVOID dll_handle = nullptr;
  NTSTATUS status = ::LdrLoadDll(nullptr, nullptr, &dll_name_us, &dll_handle);
  if (!NT_SUCCESS(status)) {
    set_last_error_from_status(status);
    return nullptr;
  }

  return static_cast<HMODULE>(dll_handle);
}

void *get_proc_address_nt(HMODULE module, const DelayLoadProc &proc) {
  PVOID proc_addr = nullptr;
  NTSTATUS status;
  if (proc.fImportByName) {
    ANSI_STRING proc_name;
    ::RtlInitString(&proc_name, proc.szProcName);
    status = ::LdrGetProcedureAddress(module, &proc_name, 0, &proc_addr);
  } else {
    status =
        ::LdrGetProcedureAddress(module, nullptr, proc.dwOrdinal, &proc_addr);
  }

  if (!NT_SUCCESS(status)) {
    set_last_error_from_status(status);
    return nullptr;
  }

  return proc_addr;
}

void unload_library_nt(HMODULE module) {
  NTSTATUS status = ::LdrUnloadDll(module);
  if (!NT_SUCCESS(status))
    set_last_error_from_status(status);
}

void *call_notify_hook(unsigned reason, DelayLoadInfo *info) {
  return __pfnDliNotifyHook2 ? __pfnDliNotifyHook2(reason, info) : nullptr;
}

//===----------------------------------------------------------------------===//
// PE structure helpers for manual resolution path
//===----------------------------------------------------------------------===//

template <typename T> T *rva_to_ptr(DWORD rva) {
  if (rva == 0)
    return nullptr;
  return reinterpret_cast<T *>(const_cast<char *>(&__ImageBase) + rva);
}

bool ascii_iequal(const char *lhs, const char *rhs) {
  if (!lhs || !rhs)
    return lhs == rhs;

  while (*lhs && *rhs) {
    if (LIBC_NAMESPACE::internal::tolower(*lhs) !=
        LIBC_NAMESPACE::internal::tolower(*rhs))
      return false;
    ++lhs;
    ++rhs;
  }
  return *lhs == *rhs;
}

//===----------------------------------------------------------------------===//
// Delay import directory traversal (for __FUnloadDelayLoadedDLL2)
//===----------------------------------------------------------------------===//

struct ImageDosHeader {
  WORD e_magic;
  WORD e_pad[29];
  long e_lfanew;
};

struct ImageDataDirectory {
  DWORD VirtualAddress;
  DWORD Size;
};

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
struct ImageOptionalHeader {
  WORD Magic;
  unsigned char pad1[14];
  unsigned long long pad2[9];
  DWORD pad3[2];
  WORD pad4[2];
  unsigned long long pad5;
  DWORD NumberOfRvaAndSizes;
  ImageDataDirectory DataDirectory[16];
};
#else
struct ImageOptionalHeader {
  WORD Magic;
  unsigned char pad1[14];
  DWORD pad2[9];
  DWORD pad3[2];
  WORD pad4[2];
  DWORD pad5;
  DWORD NumberOfRvaAndSizes;
  ImageDataDirectory DataDirectory[16];
};
#endif

struct ImageFileHeader {
  WORD Machine;
  WORD NumberOfSections;
  DWORD TimeDateStamp;
  DWORD PointerToSymbolTable;
  DWORD NumberOfSymbols;
  WORD SizeOfOptionalHeader;
  WORD Characteristics;
};

struct ImageNtHeaders {
  DWORD Signature;
  ImageFileHeader FileHeader;
  ImageOptionalHeader OptionalHeader;
};

constexpr DWORD kDelayImportDirIndex = 13;

const IMAGE_DELAYLOAD_DESCRIPTOR *get_delay_import_directory() {
  auto *dos = reinterpret_cast<const ImageDosHeader *>(&__ImageBase);
  auto *nt = reinterpret_cast<const ImageNtHeaders *>(&__ImageBase +
                                                       dos->e_lfanew);
  if (nt->OptionalHeader.NumberOfRvaAndSizes <= kDelayImportDirIndex)
    return nullptr;

  const auto &dir = nt->OptionalHeader.DataDirectory[kDelayImportDirIndex];
  if (dir.VirtualAddress == 0 || dir.Size == 0)
    return nullptr;

  return rva_to_ptr<const IMAGE_DELAYLOAD_DESCRIPTOR>(dir.VirtualAddress);
}

size_t count_iat_entries(const IMAGE_THUNK_DATA *iat) {
  size_t count = 0;
  while (iat[count].u1 != 0)
    ++count;
  return count;
}

// Restore IAT entries from the unload table, handling CFG read-only pages.
void overlay_iat(IMAGE_THUNK_DATA *dst, const IMAGE_THUNK_DATA *src,
                 size_t count) {
  if (count == 0)
    return;

  size_t size = count * sizeof(IMAGE_THUNK_DATA);
  // NtProtectVirtualMemory operates on page-aligned regions — use intermediate
  // base/size variables as it may round them.
  PVOID base = dst;
  SIZE_T sz = size;
  ULONG old_protect = 0;
  ::NtProtectVirtualMemory(NtCurrentProcess(), &base, &sz,
                            PAGE_READWRITE, &old_protect);

  for (size_t i = 0; i < count; ++i)
    dst[i].u1 = src[i].u1;

  if (old_protect != 0 && old_protect != PAGE_READWRITE) {
    PVOID base2 = dst;
    SIZE_T sz2 = size;
    ULONG unused;
    ::NtProtectVirtualMemory(NtCurrentProcess(), &base2, &sz2,
                              old_protect, &unused);
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

extern "C" {

// Called by linker-generated delay load thunks.
void *__stdcall __delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR *pidd,
                                   void **ppfnIATEntry) {
  if (!pidd->Attributes.RvaBased)
    return nullptr;

  LPCSTR dll_name = rva_to_ptr<const char>(pidd->DllNameRVA);
  HMODULE *phmod = rva_to_ptr<HMODULE>(pidd->ModuleHandleRVA);
  auto *iat = rva_to_ptr<IMAGE_THUNK_DATA>(pidd->ImportAddressTableRVA);
  auto *int_table = rva_to_ptr<IMAGE_THUNK_DATA>(pidd->ImportNameTableRVA);
  auto *iat_entry = reinterpret_cast<IMAGE_THUNK_DATA *>(ppfnIATEntry);
  size_t idx = iat_entry - iat;

  DelayLoadInfo dli = {};
  dli.cb = sizeof(dli);
  dli.pidd = pidd;
  dli.ppfn = reinterpret_cast<void **>(&iat_entry->u1);
  dli.szDll = dll_name;

  auto *int_entry = &int_table[idx];
  if (int_entry->u1 & kImageOrdinalFlag) {
    dli.dlp.fImportByName = 0;
    dli.dlp.dwOrdinal = static_cast<DWORD>(int_entry->u1 & 0xFFFF);
  } else {
    dli.dlp.fImportByName = 1;
    // Skip 2-byte hint to get name string.
    dli.dlp.szProcName =
        rva_to_ptr<const char>(static_cast<DWORD>(int_entry->u1)) + 2;
  }

  // dliStartProcessing: hook can bypass entire resolution.
  void *result = call_notify_hook(dliStartProcessing, &dli);
  if (result) {
    iat_entry->u1 = reinterpret_cast<decltype(iat_entry->u1)>(result);
    return result;
  }

  HMODULE hmod = phmod ? *phmod : nullptr;
  if (!hmod) {
    hmod = static_cast<HMODULE>(call_notify_hook(dliNotePreLoadLibrary, &dli));
    if (!hmod)
      hmod = load_library_nt(dll_name);
    if (!hmod) {
      dli.dwLastError = ::NtGetLastError();
      if (__pfnDliFailureHook2)
        hmod = static_cast<HMODULE>(
            __pfnDliFailureHook2(dliFailLoadLib, &dli));
      if (!hmod) {
        ::NtSetLastError(dli.dwLastError);
        return nullptr;
      }
    }
    // Thread-safe module handle storage.
    if (phmod) {
      HMODULE expected = nullptr;
      if (!__atomic_compare_exchange_n(phmod, &expected, hmod, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        hmod = expected;
    }
  }

  dli.hmodCur = hmod;

  void *pfn = call_notify_hook(dliNotePreGetProcAddress, &dli);
  if (!pfn)
    pfn = get_proc_address_nt(hmod, dli.dlp);

  if (!pfn) {
    dli.dwLastError = ::NtGetLastError();
    if (__pfnDliFailureHook2)
      pfn = __pfnDliFailureHook2(dliFailGetProc, &dli);
    if (!pfn) {
      ::NtSetLastError(dli.dwLastError);
      return nullptr;
    }
  }

  dli.pfnCur = pfn;
  iat_entry->u1 = reinterpret_cast<decltype(iat_entry->u1)>(pfn);

  (void)call_notify_hook(dliNoteEndProcessing, &dli);
  return pfn;
}

// Pre-load all delay imports for a specific DLL.
HRESULT __stdcall __HrLoadAllImportsForDll(LPCSTR szDll) {
  constexpr HRESULT E_MOD_NOT_FOUND = 0x8007007EL;
  const IMAGE_DELAYLOAD_DESCRIPTOR *pidd = get_delay_import_directory();
  if (!pidd)
    return S_OK;

  bool matched = false;
  for (; pidd->DllNameRVA != 0; ++pidd) {
    if (!pidd->Attributes.RvaBased)
      continue;

    LPCSTR name = rva_to_ptr<const char>(pidd->DllNameRVA);
    if (!ascii_iequal(name, szDll))
      continue;

    matched = true;
    auto *iat = rva_to_ptr<IMAGE_THUNK_DATA>(pidd->ImportAddressTableRVA);
    if (!iat)
      return E_MOD_NOT_FOUND;

    for (size_t i = 0; iat[i].u1 != 0; ++i) {
      if (!__delayLoadHelper2(pidd, reinterpret_cast<void **>(&iat[i].u1)))
        return E_MOD_NOT_FOUND;
    }
  }

  return matched ? S_OK : E_MOD_NOT_FOUND;
}

// Unload a delay-loaded DLL. Requires /DELAY:UNLOAD at link time.
int __stdcall __FUnloadDelayLoadedDLL2(LPCSTR szDll) {
  const IMAGE_DELAYLOAD_DESCRIPTOR *pidd = get_delay_import_directory();
  if (!pidd)
    return 0;

  for (; pidd->DllNameRVA != 0; ++pidd) {
    if (!pidd->Attributes.RvaBased)
      continue;

    LPCSTR name = rva_to_ptr<const char>(pidd->DllNameRVA);
    if (!ascii_iequal(name, szDll))
      continue;

    if (pidd->UnloadInformationTableRVA == 0)
      return 0;

    HMODULE *phmod = rva_to_ptr<HMODULE>(pidd->ModuleHandleRVA);
    if (!phmod)
      return 0;

    HMODULE hmod = __atomic_exchange_n(phmod, nullptr, __ATOMIC_ACQ_REL);
    if (!hmod)
      return 0;

    unload_library_nt(hmod);

    auto *iat_dst =
        rva_to_ptr<IMAGE_THUNK_DATA>(pidd->ImportAddressTableRVA);
    auto *iat_src = rva_to_ptr<const IMAGE_THUNK_DATA>(
        pidd->UnloadInformationTableRVA);

    if (iat_dst && iat_src)
      overlay_iat(iat_dst, iat_src, count_iat_entries(iat_src));

    return 1;
  }

  return 0;
}

} // extern "C"
