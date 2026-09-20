//===-- delayload.cpp - Delay-load helper ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The linker's /delayload thunks call __delayLoadHelper2. Resolution goes
// through the loader's own ResolveDelayLoadedAPI, which understands protected
// delay-load import address tables, api set redirection, the application
// verifier and the shim engine, and which reaches the system failure hook.
// That hook answers a call into a component that is not installed with a
// substitute stub, so such a call returns a failure code instead of crashing.
//
// A failure nothing substitutes for is reported the way delayimp.h specifies:
// the image's failure hook is asked, and then a structured exception carrying
// a DelayLoadInfo is raised. The loader's own noncontinuable exception is
// caught here so that only one of the two contracts reaches the program.
//
// Programs that install the delayimp.h notify hook take a manual path instead,
// so the hook sees every stage. That path writes the import address table
// itself, opening the page around the store when the table is a protected
// one. The failure hook is consulted on both paths.
//
// __FUnloadDelayLoadedDLL2 needs more than the copy of the address table that
// /delay:unload puts in the image. A library the loader loads for a delayed
// import becomes a dependency of this image rather than a reference this image
// holds, and FreeLibrary does not undo a dependency -- it reports success and
// the library stays. So a descriptor that carries an unload table is resolved
// by this file, which loads the library itself and owns the reference that
// frees it. That is the bargain /delay:unload makes, and it is why MSVC loads
// in the image too.
//
// The hook types below are the delayimp.h ABI. That header ships only with
// the MSVC tools, so it is not included; the Windows SDK's own delay-load
// callback declarations use a different signature and are not used either.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <string.h>

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

// Loader interface (Windows 8 and later, forwarded to ntdll). The system
// failure hook is kernel32's, whose body holds the substitute-stub table.
using DelayLoadSystemHook = void *(__stdcall *)(LPCSTR, LPCSTR);

extern "C" {
__declspec(dllimport) void *__stdcall
ResolveDelayLoadedAPI(void *, const IMAGE_DELAYLOAD_DESCRIPTOR *, void *,
                      DelayLoadSystemHook, IMAGE_THUNK_DATA *, DWORD);
__declspec(dllimport) LONG __stdcall ResolveDelayLoadsFromDll(void *, LPCSTR,
                                                              DWORD);
__declspec(dllimport) void *__stdcall DelayLoadFailureHook(LPCSTR, LPCSTR);

extern IMAGE_DOS_HEADER __ImageBase;
extern char __guard_flags[];

// loadconfig.cpp
WORD __wincrt_dependent_load_flags(void);
}

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

// The loader refuses a resolution whose flags name anything but a search path,
// so only those are passed on: the DLL load directory, the application
// directory, System32, the default directories, the safe current directories,
// and System32 without forwarders. The user directories need a process-wide
// policy this image cannot see, and are left out rather than risking refusal.
constexpr DWORD SearchPathFlags = 0x7b00;

// delayimp.h reports failures as Visual C++ facility exceptions.
constexpr DWORD vcppException(DWORD Error) {
  return ERROR_SEVERITY_ERROR | (0x6dUL << 16) | Error;
}

bool delayIatIsProtected() {
  return (reinterpret_cast<uintptr_t>(__guard_flags) &
          IMAGE_GUARD_PROTECT_DELAYLOAD_IAT) != 0;
}

template <typename T> T *fromRva(DWORD Rva) {
  return Rva ? reinterpret_cast<T *>(reinterpret_cast<char *>(&__ImageBase) +
                                     Rva)
             : nullptr;
}

// A protected table is read-only except while the loader resolves into it, so
// a substitute this image supplies has to open the page itself.
void storeSlot(IMAGE_THUNK_DATA *Entry, void *Value) {
  DWORD Old;
  bool Protected = delayIatIsProtected();
  if (Protected && !VirtualProtect(Entry, sizeof(*Entry), PAGE_READWRITE, &Old))
    return;
  Entry->u1.Function = reinterpret_cast<ULONG_PTR>(Value);
  if (Protected)
    VirtualProtect(Entry, sizeof(*Entry), PAGE_READONLY, &Old);
}

// The loader reads the module handle slot before it takes the load path, and
// writes it only while the table is unprotected. Recording it spares every
// later descriptor naming the same library that path, and the dependency it
// would add. The handle carries no reference of its own.
void cacheModule(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor) {
  auto *Slot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  if (!Slot || *Slot)
    return;
  HMODULE Module = GetModuleHandleA(fromRva<const char>(Descriptor->DllNameRVA));
  if (!Module)
    return;
  HMODULE Expected = nullptr;
  __atomic_compare_exchange_n(Slot, &Expected, Module, false, __ATOMIC_ACQ_REL,
                              __ATOMIC_ACQUIRE);
}

// The descriptor's two tables are parallel, so a slot's index in the import
// address table is its name's index in the name table.
void describeImport(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                    IMAGE_THUNK_DATA *Entry, DelayLoadInfo &Dli) {
  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Names = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportNameTableRVA);
  const IMAGE_THUNK_DATA &Name = Names[Entry - Iat];

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
}

void *notify(unsigned Reason, DelayLoadInfo &Dli) {
  return __pfnDliNotifyHook2 ? __pfnDliNotifyHook2(Reason, &Dli) : nullptr;
}

// This image's own delay-load directory, searched by the name the import
// library wrote. The comparison ignores case, as the loader's own descriptor
// lookup does.
const IMAGE_DELAYLOAD_DESCRIPTOR *findDescriptor(LPCSTR DllName) {
  auto *Nt =
      fromRva<IMAGE_NT_HEADERS>(static_cast<DWORD>(__ImageBase.e_lfanew));
  DWORD Rva = Nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                  .VirtualAddress;
  if (!Rva)
    return nullptr;
  for (auto *D = fromRva<IMAGE_DELAYLOAD_DESCRIPTOR>(Rva); D->DllNameRVA; ++D)
    if (!_stricmp(fromRva<const char>(D->DllNameRVA), DllName))
      return D;
  return nullptr;
}

LPCSTR procNameOrOrdinal(const DelayLoadInfo &Dli) {
  return Dli.dlp.fImportByName ? Dli.dlp.szProcName
                               : reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(
                                     Dli.dlp.dwOrdinal));
}

// The calling stub has nowhere to put an error, so a failure no hook answers
// becomes an exception. A filter may still resume the raise, in which case the
// helper returns null as delayimp.h documents.
void *raiseFailure(DWORD Error, DelayLoadInfo &Dli) {
  ULONG_PTR Args[1] = {reinterpret_cast<ULONG_PTR>(&Dli)};
  SetLastError(Dli.dwLastError);
  RaiseException(vcppException(Error), 0, 1, Args);
  return nullptr;
}

// Everything the loader tried has failed. The two stages are told apart by
// whether the module is loaded, because the loader reports one reason for
// both, and a protected image never caches the module handle.
void *reportFailure(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                    IMAGE_THUNK_DATA *Entry) {
  DelayLoadInfo Dli = {};
  describeImport(Descriptor, Entry, Dli);
  Dli.dwLastError = GetLastError();

  auto *ModuleSlot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  Dli.hmodCur =
      ModuleSlot && *ModuleSlot ? *ModuleSlot : GetModuleHandleA(Dli.szDll);
  if (!Dli.hmodCur) {
    if (!__pfnDliFailureHook2)
      return raiseFailure(ERROR_MOD_NOT_FOUND, Dli);
    // A hook that answers this stage returns a module it loaded itself, and
    // the procedure still has to be found in it.
    Dli.hmodCur =
        static_cast<HMODULE>(__pfnDliFailureHook2(dliFailLoadLib, &Dli));
    if (!Dli.hmodCur)
      return raiseFailure(ERROR_MOD_NOT_FOUND, Dli);
    if (void *Proc = reinterpret_cast<void *>(
            GetProcAddress(Dli.hmodCur, procNameOrOrdinal(Dli)))) {
      Dli.pfnCur = Proc;
      storeSlot(Entry, Proc);
      return Proc;
    }
    Dli.dwLastError = GetLastError();
  }

  if (__pfnDliFailureHook2)
    if (void *Proc = __pfnDliFailureHook2(dliFailGetProc, &Dli)) {
      Dli.pfnCur = Proc;
      storeSlot(Entry, Proc);
      return Proc;
    }
  return raiseFailure(ERROR_PROC_NOT_FOUND, Dli);
}

// Loading and resolving in the image, which a notify hook needs in order to
// see every stage and an unloadable library needs in order to be freed.
void *resolveInImage(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                     IMAGE_THUNK_DATA *Entry) {
  DelayLoadInfo Dli = {};
  describeImport(Descriptor, Entry, Dli);

  if (void *Result = notify(dliStartProcessing, Dli)) {
    storeSlot(Entry, Result);
    return Result;
  }

  auto *ModuleSlot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  HMODULE Module = ModuleSlot ? *ModuleSlot : nullptr;
  if (!Module) {
    Module = static_cast<HMODULE>(notify(dliNotePreLoadLibrary, Dli));
    if (!Module)
      Module = LoadLibraryExA(Dli.szDll, nullptr,
                              __wincrt_dependent_load_flags() & SearchPathFlags);
    if (!Module)
      return reportFailure(Descriptor, Entry);
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

  void *Function = notify(dliNotePreGetProcAddress, Dli);
  if (!Function)
    Function =
        reinterpret_cast<void *>(GetProcAddress(Module, procNameOrOrdinal(Dli)));
  if (!Function)
    return reportFailure(Descriptor, Entry);

  Dli.pfnCur = Function;
  storeSlot(Entry, Function);
  notify(dliNoteEndProcessing, Dli);
  return Function;
}

} // namespace

extern "C" {

void *__stdcall __delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                                   void **Entry) {
  auto *Thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(Entry);
  if (!Descriptor->Attributes.RvaBased) {
    DelayLoadInfo Dli = {};
    Dli.cb = sizeof(Dli);
    Dli.pidd = Descriptor;
    Dli.ppfn = Entry;
    return raiseFailure(ERROR_INVALID_PARAMETER, Dli);
  }

  if (__pfnDliNotifyHook2 || Descriptor->UnloadInformationTableRVA)
    return resolveInImage(Descriptor, Thunk);

  void *Proc = nullptr;
  __try {
    Proc = ResolveDelayLoadedAPI(
        &__ImageBase, Descriptor, nullptr,
        DelayLoadFailureHook, Thunk,
        __wincrt_dependent_load_flags() & SearchPathFlags);
  } __except (GetExceptionCode() == ERROR_DELAY_LOAD_FAILED
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    Proc = nullptr;
  }
  if (!Proc)
    return reportFailure(Descriptor, Thunk);
  cacheModule(Descriptor);
  return Proc;
}

// Restores the address table this image started with and frees the library, so
// that a later call loads it again. FALSE if the DLL is not delay-loaded by
// this image, if it was never loaded, or if the image was linked without
// /delay:unload and so carries no copy of the table to restore.
//
// The name is compared without regard to case, as the loader compares it in
// its own descriptor lookup.
BOOL __stdcall __FUnloadDelayLoadedDLL2(LPCSTR DllName) {
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor = findDescriptor(DllName);
  if (!Descriptor || !Descriptor->UnloadInformationTableRVA)
    return FALSE;
  auto *Slot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  HMODULE Module = Slot ? *Slot : nullptr;
  if (!Module)
    return FALSE;

  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Saved =
      fromRva<IMAGE_THUNK_DATA>(Descriptor->UnloadInformationTableRVA);
  SIZE_T Count = 0;
  while (Saved[Count].u1.Function)
    ++Count;

  // The stubs go back before the library does, so a call racing this one
  // reaches the helper rather than a freed page.
  DWORD Old;
  SIZE_T Bytes = Count * sizeof(*Iat);
  bool Protected = delayIatIsProtected();
  if (Protected && !VirtualProtect(Iat, Bytes, PAGE_READWRITE, &Old))
    return FALSE;
  for (SIZE_T I = 0; I != Count; ++I)
    Iat[I].u1.Function = Saved[I].u1.Function;
  if (Protected)
    VirtualProtect(Iat, Bytes, PAGE_READONLY, &Old);

  *Slot = nullptr;
  FreeLibrary(Module);
  return TRUE;
}

// Loads every delayed import of one DLL now. Hooks are not consulted, as with
// vcruntime: the loader resolves the whole descriptor with none installed.
HRESULT __stdcall __HrLoadAllImportsForDll(LPCSTR DllName) {
  // An unloadable library has to be loaded by this image, so that the handle
  // __FUnloadDelayLoadedDLL2 frees is the one that holds it.
  if (const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor = findDescriptor(DllName)) {
    if (Descriptor->UnloadInformationTableRVA) {
      auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
      for (SIZE_T I = 0; Iat[I].u1.Function; ++I)
        if (!__delayLoadHelper2(Descriptor,
                                reinterpret_cast<void **>(&Iat[I])))
          return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
      return S_OK;
    }
  }

  LONG Status =
      ResolveDelayLoadsFromDll(&__ImageBase, DllName, 0);
  if (Status >= 0)
    return S_OK;
  // The DLL not being a delay-load dependency of this image is reported apart
  // from an import of it that did not resolve.
  return static_cast<DWORD>(Status) == STATUS_DLL_NOT_FOUND
             ? HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
             : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

} // extern "C"
