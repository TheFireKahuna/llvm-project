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

// Hooks are invoked by linker-generated delay-load thunks with MS x64 ABI.
// `decltype` on an unreferenced external prototype — LIBC_MSABI attaches
// to the declaration; the trailing-alias spelling trips `-Wgcc-compat`.
LIBC_MSABI void *__dli_hook_type_source(unsigned, DelayLoadInfo *);
using PfnDliHook = decltype(&__dli_hook_type_source);

enum {
  dliStartProcessing = 0,
  dliNotePreLoadLibrary = 1,
  dliNotePreGetProcAddress = 2,
  dliFailLoadLib = 3,
  dliFailGetProc = 4,
  dliNoteEndProcessing = 5,
};

// Hook pointers — const (.rdata) by default to prevent hijacking. Emitted
// as COFF weak externals aliased to null defaults: user code overrides
// via a strong definition; otherwise the linker resolves each pointer to
// its `_default` null via the aux record.
extern "C" {

extern const PfnDliHook __wincrt_dli_notify_hook_default = nullptr;
extern const PfnDliHook __wincrt_dli_failure_hook_default = nullptr;

__attribute__((weak, alias("__wincrt_dli_notify_hook_default")))
extern const PfnDliHook __pfnDliNotifyHook2;
__attribute__((weak, alias("__wincrt_dli_failure_hook_default")))
extern const PfnDliHook __pfnDliFailureHook2;

} // extern "C"

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

// Process-wide IAT-overlay lock: protects the unprotect/write/reprotect
// window. Without it, two threads delay-loading distinct entries that
// share a 4K IAT page can race the protection toggle and leave the
// page in the wrong state. Plain TTAS spinlock (freestanding TU — no
// Futex/RawMutex available); contention is bounded to the first call
// per delayload symbol.
static volatile long g_iat_overlay_lock = 0;

static void iat_overlay_lock_acquire() {
  while (__atomic_exchange_n(&g_iat_overlay_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    while (__atomic_load_n(&g_iat_overlay_lock, __ATOMIC_RELAXED) != 0) {
#if defined(__x86_64__)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      __asm__ __volatile__("yield" ::: "memory");
#endif
    }
  }
}

static void iat_overlay_lock_release() {
  __atomic_store_n(&g_iat_overlay_lock, 0, __ATOMIC_RELEASE);
}

// Restore IAT entries from the unload table, handling CFG read-only pages.
void overlay_iat(IMAGE_THUNK_DATA *dst, const IMAGE_THUNK_DATA *src,
                 size_t count) {
  if (count == 0)
    return;

  iat_overlay_lock_acquire();

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

  iat_overlay_lock_release();
}

} // namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

extern "C" {

// Called by linker-generated delay load thunks with MS x64 ABI.
LIBC_MSABI void *__delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR *pidd,
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
    // Thread-safe module handle storage. CAS loser drops its
    // LdrLoadDll refcount: without this the loser's handle would stay
    // pinned for process life. LdrUnloadDll only decrements the
    // refcount (the winner's load already bumped it once and that
    // counts as the long-lived reference).
    if (phmod) {
      HMODULE expected = nullptr;
      if (!__atomic_compare_exchange_n(phmod, &expected, hmod, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ::LdrUnloadDll(hmod);
        hmod = expected;
      }
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
LIBC_MSABI HRESULT __HrLoadAllImportsForDll(LPCSTR szDll) {
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
LIBC_MSABI int __FUnloadDelayLoadedDLL2(LPCSTR szDll) {
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
