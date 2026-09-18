//===-- delayload.cpp - Delay-load helper ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The linker's /delayload thunks call __delayLoadHelper2. Resolution normally
// goes through the loader's own ResolveDelayLoadedAPI, which understands
// protected delay-load IATs. Programs that install the delayimp.h notify hook
// take the manual path so the hook sees every stage, as with vcruntime.
//
// The hook types below are the delayimp.h ABI. That header ships only with
// the MSVC tools, so it is not included; the Windows SDK's own delay-load
// callback declarations use a different signature and are not used either.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

namespace {

// Loader interface (Windows 8 and later, forwarded to ntdll).
struct NativeProcDescriptor {
  DWORD ImportDescribedByName;
  union {
    LPCSTR Name;
    DWORD Ordinal;
  } Description;
};

struct NativeLoadInfo {
  DWORD Size;
  const IMAGE_DELAYLOAD_DESCRIPTOR *DelayloadDescriptor;
  IMAGE_THUNK_DATA *ThunkAddress;
  LPCSTR TargetDllName;
  NativeProcDescriptor TargetApiDescriptor;
  void *TargetModuleBase;
  void *Unused;
  DWORD LastError;
};

constexpr DWORD NativeGetProcFailure = 4;

using NativeFailureCallback = void *(__stdcall *)(DWORD, NativeLoadInfo *);

} // namespace

extern "C" {
__declspec(dllimport) void *__stdcall
ResolveDelayLoadedAPI(void *, const IMAGE_DELAYLOAD_DESCRIPTOR *,
                      NativeFailureCallback, void *, IMAGE_THUNK_DATA *, DWORD);
__declspec(dllimport) LONG __stdcall ResolveDelayLoadsFromDll(void *, LPCSTR,
                                                              DWORD);
extern const char __ImageBase;
}

// delayimp.h ABI.
struct DelayLoadProc {
  BOOL fImportByName;
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

// Hooks live in .rdata so memory corruption cannot redirect them. The
// fallback needs external linkage for the alternate name to resolve to it.
extern "C" {
extern const PfnDliHook __pfnDliNotifyHook2;
extern const PfnDliHook __pfnDliFailureHook2;
extern const PfnDliHook __wincrt_no_dli_hook = nullptr;
}
WINCRT_ALTERNATENAME(__pfnDliNotifyHook2, __wincrt_no_dli_hook)
WINCRT_ALTERNATENAME(__pfnDliFailureHook2, __wincrt_no_dli_hook)

namespace {

template <typename T> T *fromRva(DWORD Rva) {
  return Rva ? reinterpret_cast<T *>(const_cast<char *>(&__ImageBase) + Rva)
             : nullptr;
}

void *__stdcall failureAdapter(DWORD Reason, NativeLoadInfo *Info) {
  DelayLoadInfo Dli = {};
  Dli.cb = sizeof(Dli);
  Dli.pidd = Info->DelayloadDescriptor;
  Dli.ppfn = reinterpret_cast<void **>(Info->ThunkAddress);
  Dli.szDll = Info->TargetDllName;
  Dli.dlp.fImportByName = Info->TargetApiDescriptor.ImportDescribedByName;
  if (Dli.dlp.fImportByName)
    Dli.dlp.szProcName = Info->TargetApiDescriptor.Description.Name;
  else
    Dli.dlp.dwOrdinal = Info->TargetApiDescriptor.Description.Ordinal;
  Dli.hmodCur = static_cast<HMODULE>(Info->TargetModuleBase);
  Dli.dwLastError = Info->LastError;
  return __pfnDliFailureHook2(
      Reason == NativeGetProcFailure ? dliFailGetProc : dliFailLoadLib, &Dli);
}

void *resolveWithHooks(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                       IMAGE_THUNK_DATA *Entry) {
  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Names = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportNameTableRVA);
  const IMAGE_THUNK_DATA &Name = Names[Entry - Iat];

  DelayLoadInfo Dli = {};
  Dli.cb = sizeof(Dli);
  Dli.pidd = Descriptor;
  Dli.ppfn = reinterpret_cast<void **>(&Entry->u1.Function);
  Dli.szDll = fromRva<const char>(Descriptor->DllNameRVA);
  if (IMAGE_SNAP_BY_ORDINAL(Name.u1.Ordinal)) {
    Dli.dlp.dwOrdinal = IMAGE_ORDINAL(Name.u1.Ordinal);
  } else {
    Dli.dlp.fImportByName = TRUE;
    Dli.dlp.szProcName =
        fromRva<IMAGE_IMPORT_BY_NAME>(static_cast<DWORD>(Name.u1.AddressOfData))
            ->Name;
  }

  if (void *Result = __pfnDliNotifyHook2(dliStartProcessing, &Dli)) {
    Entry->u1.Function = reinterpret_cast<ULONG_PTR>(Result);
    return Result;
  }

  auto *ModuleSlot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  HMODULE Module = ModuleSlot ? *ModuleSlot : nullptr;
  if (!Module) {
    Module =
        static_cast<HMODULE>(__pfnDliNotifyHook2(dliNotePreLoadLibrary, &Dli));
    if (!Module)
      Module = LoadLibraryA(Dli.szDll);
    if (!Module) {
      Dli.dwLastError = GetLastError();
      if (__pfnDliFailureHook2)
        Module =
            static_cast<HMODULE>(__pfnDliFailureHook2(dliFailLoadLib, &Dli));
      if (!Module) {
        SetLastError(Dli.dwLastError);
        return nullptr;
      }
    }
    if (ModuleSlot) {
      HMODULE Expected = nullptr;
      // Another thread may have loaded the module first; keep one handle.
      if (!__atomic_compare_exchange_n(ModuleSlot, &Expected, Module, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        FreeLibrary(Module);
        Module = Expected;
      }
    }
  }
  Dli.hmodCur = Module;

  void *Function = __pfnDliNotifyHook2(dliNotePreGetProcAddress, &Dli);
  if (!Function)
    Function = reinterpret_cast<void *>(GetProcAddress(
        Module, Dli.dlp.fImportByName
                    ? Dli.dlp.szProcName
                    : reinterpret_cast<LPCSTR>(
                          static_cast<uintptr_t>(Dli.dlp.dwOrdinal))));
  if (!Function) {
    Dli.dwLastError = GetLastError();
    if (__pfnDliFailureHook2)
      Function = __pfnDliFailureHook2(dliFailGetProc, &Dli);
    if (!Function) {
      SetLastError(Dli.dwLastError);
      return nullptr;
    }
  }

  Dli.pfnCur = Function;
  Entry->u1.Function = reinterpret_cast<ULONG_PTR>(Function);
  __pfnDliNotifyHook2(dliNoteEndProcessing, &Dli);
  return Function;
}

} // namespace

extern "C" {

void *__stdcall __delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                                   void **Entry) {
  auto *Thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(Entry);
  if (__pfnDliNotifyHook2 && Descriptor->Attributes.RvaBased)
    return resolveWithHooks(Descriptor, Thunk);
  return ResolveDelayLoadedAPI(const_cast<char *>(&__ImageBase), Descriptor,
                               __pfnDliFailureHook2 ? failureAdapter : nullptr,
                               nullptr, Thunk, 0);
}

// Loads every delayed import of one DLL now. Notify hooks are not consulted,
// as with vcruntime.
HRESULT __stdcall __HrLoadAllImportsForDll(LPCSTR DllName) {
  LONG Status =
      ResolveDelayLoadsFromDll(const_cast<char *>(&__ImageBase), DllName, 0);
  constexpr HRESULT ModuleNotFound =
      0x8007007E; // HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
  return static_cast<DWORD>(Status) == STATUS_DLL_NOT_FOUND ? ModuleNotFound
                                                            : S_OK;
}

} // extern "C"
