//===-- delayload.cpp - The delay-load helper -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The linker's /delayload thunks call __delayLoadHelper2. It resolves through
// the loader's ResolveDelayLoadedAPI, which knows protected tables, API sets,
// the application verifier and the shim engine, and which reaches kernel32's
// failure hook: that hook answers a call into a component that is not
// installed with a stub that returns a failure code.
//
// A library the loader loads for a delayed import becomes a dependency of the
// image, which FreeLibrary does not undo, and the loader shows no hook the
// stages of its work. So a descriptor with an unload table, which the linker
// writes under /delay:unload, and every descriptor of an image with
// delayimp.h's notify hook, is resolved in the image instead, import by
// import, as delayimp.h describes. Both are properties of the whole image, so
// the loader never writes a table that this file writes.
//
// A failure is reported as delayimp.h specifies on either path: the image's
// failure hook is asked, and then an exception that carries a DelayLoadInfo is
// raised.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <delayimp.h>
#include <string.h>

static_assert(sizeof(ImgDelayDescr) == sizeof(IMAGE_DELAYLOAD_DESCRIPTOR),
              "delayimp.h's descriptor is the SDK's");

extern "C" {
// Exported by kernel32, the first forwarded to ntdll; no SDK header declares
// them. The failure hook is the system's, which has the substitute stubs.
__declspec(dllimport) PVOID WINAPI ResolveDelayLoadedAPI(
    PVOID ParentModuleBase, const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
    PVOID FailureDllHook, PVOID(WINAPI *FailureSystemHook)(LPCSTR, LPCSTR),
    IMAGE_THUNK_DATA *ThunkAddress, ULONG Flags);
__declspec(dllimport) PVOID WINAPI DelayLoadFailureHook(LPCSTR DllName,
                                                        LPCSTR ProcName);

// The builtins' binding of what a library's list names (fill_windows.c).
void __llvm_delay_resolve(const void *List);

// Defined in assembly, and read as volatile so that the linker's values are.
extern const volatile IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used;

// Tells the linker that this helper honours a protected table, which the
// linker then gives every image that links it: the loader brackets its own
// writes, and this file's are bracketed below.
extern const char __MSSafeDelayLoadIAT = 0;

// The hooks, and the section-wide protection choice, of a program that
// defines none. The choice is never read: only the pages written are opened.
extern const PfnDliHook __wincrt_no_dli_hook = nullptr;
extern const BOOL __wincrt_no_whole_dload_section = FALSE;
}
WINCRT_ALTERNATENAME(__pfnDliNotifyHook2, __wincrt_no_dli_hook)
WINCRT_ALTERNATENAME(__pfnDliFailureHook2, __wincrt_no_dli_hook)
WINCRT_ALTERNATENAME(__bChangeProtectionOfWholeDloadSection,
                     __wincrt_no_whole_dload_section)

namespace {

template <typename T> T *fromRva(DWORD Rva) {
  return reinterpret_cast<T *>(reinterpret_cast<char *>(&__ImageBase) + Rva);
}

// The search path the image asks for through /dependentloadflag, which the
// loader applies to the eager imports only. It refuses a resolution whose
// flags name anything else, and the user directories need a process-wide
// policy this image cannot see, so only the other search-path flags pass.
DWORD searchPathFlags() {
  return _load_config_used.DependentLoadFlags & 0x7b00;
}

// Whether a slot still holds its initial value, the address of the linker's
// thunk in this image, as the loader decides it.
bool isUnbound(const IMAGE_THUNK_DATA &Slot) {
  return __atomic_load_n(&Slot.u1.Function, __ATOMIC_RELAXED) -
             reinterpret_cast<ULONG_PTR>(&__ImageBase) <
         wincrt::imageSize(reinterpret_cast<HMODULE>(&__ImageBase));
}

// A protected table is read-only except while it is written, and only the
// pages written are opened. Two threads may write slots on one page, and one
// must not restore the protection while the other is still storing, so the
// image serializes its writes. Other threads call through a slot while it is
// written, so each store is atomic.
SRWLOCK TableLock = SRWLOCK_INIT;

void writeSlots(IMAGE_THUNK_DATA *Slots, const IMAGE_THUNK_DATA *Values,
                size_t Count) {
  AcquireSRWLockExclusive(&TableLock);
  DWORD Protection;
  if (VirtualProtect(Slots, Count * sizeof(*Slots), PAGE_READWRITE,
                     &Protection)) {
    for (size_t I = 0; I != Count; ++I)
      __atomic_store_n(&Slots[I].u1.Function, Values[I].u1.Function,
                       __ATOMIC_RELAXED);
    VirtualProtect(Slots, Count * sizeof(*Slots), Protection, &Protection);
  }
  ReleaseSRWLockExclusive(&TableLock);
}

FARPROC notify(unsigned Stage, DelayLoadInfo &Info) {
  return __pfnDliNotifyHook2 ? __pfnDliNotifyHook2(Stage, &Info) : nullptr;
}

// The image's failure hook answers with the library or the procedure that
// could not be found. The calling thunk has nowhere to put an error, so a
// failure that the hook does not answer becomes an exception, whose handler
// may resume it with the answer in pfnCur.
FARPROC fail(unsigned Stage, DWORD Error, DelayLoadInfo &Info) {
  Info.dwLastError = GetLastError();
  if (__pfnDliFailureHook2)
    if (FARPROC Answer = __pfnDliFailureHook2(Stage, &Info))
      return Answer;
  ULONG_PTR Arguments[] = {reinterpret_cast<ULONG_PTR>(&Info)};
  RaiseException(VcppException(ERROR_SEVERITY_ERROR, Error), 0, 1, Arguments);
  return Info.pfnCur;
}

// Resolves one import, stage by stage as delayimp.h describes. With Own set
// the image loads the library, keeping one handle in the descriptor's slot,
// and stores the result. Without it the loader has already failed, with one
// reason for both stages, and the stages only find which to report: this
// image must not store into a table the loader writes.
FARPROC resolve(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                IMAGE_THUNK_DATA *Slot, bool Own) {
  // The descriptor's two tables are parallel.
  const IMAGE_THUNK_DATA &Name = fromRva<const IMAGE_THUNK_DATA>(
      Descriptor
          ->ImportNameTableRVA)[Slot - fromRva<IMAGE_THUNK_DATA>(
                                           Descriptor->ImportAddressTableRVA)];
  DelayLoadInfo Info = {};
  Info.cb = sizeof(Info);
  Info.pidd = reinterpret_cast<PCImgDelayDescr>(Descriptor);
  Info.ppfn = reinterpret_cast<FARPROC *>(Slot);
  Info.szDll = fromRva<const char>(Descriptor->DllNameRVA);
  // What GetProcAddress takes: the name, or the ordinal as a pointer.
  LPCSTR Procedure;
  if (IMAGE_SNAP_BY_ORDINAL(Name.u1.Ordinal)) {
    Info.dlp.dwOrdinal = IMAGE_ORDINAL(Name.u1.Ordinal);
    Procedure = MAKEINTRESOURCEA(Info.dlp.dwOrdinal);
  } else {
    Info.dlp.fImportByName = TRUE;
    Info.dlp.szProcName = Procedure =
        fromRva<IMAGE_IMPORT_BY_NAME>(Name.u1.AddressOfData)->Name;
  }

  // An answer to dliStartProcessing is where the call goes, and is not stored.
  FARPROC Result = notify(dliStartProcessing, Info);
  if (!Result) {
    auto *Handle = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
    HMODULE Module = __atomic_load_n(Handle, __ATOMIC_ACQUIRE);
    if (!Module) {
      Module = reinterpret_cast<HMODULE>(notify(dliNotePreLoadLibrary, Info));
      if (!Module)
        Module = Own ? LoadLibraryExA(Info.szDll, nullptr, searchPathFlags())
                     : GetModuleHandleA(Info.szDll);
      if (!Module)
        Module = reinterpret_cast<HMODULE>(
            fail(dliFailLoadLib, ERROR_MOD_NOT_FOUND, Info));
      if (!Module)
        return nullptr;
      // Another thread may have loaded the library first.
      HMODULE Expected = nullptr;
      if (Own &&
          !__atomic_compare_exchange_n(Handle, &Expected, Module, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        FreeLibrary(Module);
        Module = Expected;
      }
    }
    Info.hmodCur = Module;
    Result = notify(dliNotePreGetProcAddress, Info);
    if (!Result)
      Result = GetProcAddress(Module, Procedure);
    if (!Result)
      Result = fail(dliFailGetProc, ERROR_PROC_NOT_FOUND, Info);
    if (!Result)
      return nullptr;
    if (Own) {
      IMAGE_THUNK_DATA Value;
      Value.u1.Function = reinterpret_cast<ULONG_PTR>(Result);
      writeSlots(Slot, &Value, 1);
    }
  }
  Info.pfnCur = Result;
  notify(dliNoteEndProcessing, Info);
  return Result;
}

// This image's descriptor for a library, by the name the import library
// wrote, compared without regard to case as the loader compares it.
const IMAGE_DELAYLOAD_DESCRIPTOR *findDescriptor(LPCSTR DllName) {
  auto *Headers = fromRva<const IMAGE_NT_HEADERS>(__ImageBase.e_lfanew);
  if (DWORD Rva = Headers->OptionalHeader
                      .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                      .VirtualAddress)
    for (auto *D = fromRva<const IMAGE_DELAYLOAD_DESCRIPTOR>(Rva);
         D->DllNameRVA; ++D)
      if (!_stricmp(fromRva<const char>(D->DllNameRVA), DllName))
        return D;
  return nullptr;
}

} // namespace

extern "C" {

// Every resolution of an import goes through here.
FARPROC WINAPI __delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                                  FARPROC *Entry) {
  auto *Slot = reinterpret_cast<IMAGE_THUNK_DATA *>(Entry);
  if (__pfnDliNotifyHook2 || Descriptor->UnloadInformationTableRVA)
    return resolve(Descriptor, Slot, /*Own=*/true);
  // The system hook raises a noncontinuable exception for a failure it has no
  // stub for, which is caught so that only delayimp.h's reaches the program.
  __try {
    if (PVOID Result = ResolveDelayLoadedAPI(&__ImageBase, Descriptor, nullptr,
                                             DelayLoadFailureHook, Slot,
                                             searchPathFlags()))
      return reinterpret_cast<FARPROC>(Result);
  } __except (GetExceptionCode() == ERROR_DELAY_LOAD_FAILED) {
  }
  return resolve(Descriptor, Slot, /*Own=*/false);
}

// Resolves every delayed import of one library now, reporting a failure as a
// call through the import would. The result is an error only for a library the
// image does not delay-load, or a failure whose exception a handler resumed.
//
// A library whose functions' addresses are its own has several descriptors:
// its own first, and one for each run of read-only words that hold such an
// address, whose table goes on to the next zero word and names the ordinal
// 0xFFFF for a word that is no import. The descriptor of the library's gate
// is the one that names the library's list where a bound table would be, with
// no time stamp. The gate tells that every word of the library is bound, the
// writable ones included, which only the list names, so it is left to the
// builtins, which bind it last.
HRESULT WINAPI __HrLoadAllImportsForDll(LPCSTR DllName) {
  auto *Headers = fromRva<const IMAGE_NT_HEADERS>(__ImageBase.e_lfanew);
  DWORD Rva =
      Headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
          .VirtualAddress;
  HRESULT Result = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  const void *List = nullptr;
  for (auto *D = fromRva<const IMAGE_DELAYLOAD_DESCRIPTOR>(Rva);
       Rva && D->DllNameRVA; ++D) {
    if (_stricmp(fromRva<const char>(D->DllNameRVA), DllName))
      continue;
    Result = S_OK;
    if (D->BoundImportAddressTableRVA && !D->TimeDateStamp) {
      List = fromRva<const void>(D->BoundImportAddressTableRVA);
      continue;
    }
    auto *Slots = fromRva<IMAGE_THUNK_DATA>(D->ImportAddressTableRVA);
    auto *Names = fromRva<const IMAGE_THUNK_DATA>(D->ImportNameTableRVA);
    for (size_t I = 0; Slots[I].u1.Function; ++I)
      if (isUnbound(Slots[I]) &&
          Names[I].u1.Ordinal != (IMAGE_ORDINAL_FLAG | 0xffff) &&
          !__delayLoadHelper2(D, reinterpret_cast<FARPROC *>(&Slots[I])))
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }
  if (List)
    __llvm_delay_resolve(List);
  return Result;
}

// Restores the table the image started with and frees the library, so that a
// later call loads it again. FALSE if the image does not delay-load the
// library, has not loaded it, or was linked without /delay:unload and so has
// no copy of the table to restore.
BOOL WINAPI __FUnloadDelayLoadedDLL2(LPCSTR DllName) {
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor = findDescriptor(DllName);
  if (!Descriptor || !Descriptor->UnloadInformationTableRVA)
    return FALSE;
  auto *Handle = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  if (!__atomic_load_n(Handle, __ATOMIC_ACQUIRE))
    return FALSE;
  auto *Saved =
      fromRva<const IMAGE_THUNK_DATA>(Descriptor->UnloadInformationTableRVA);
  size_t Count = 0;
  while (Saved[Count].u1.Function)
    ++Count;
  // The thunks go back before the library does, so that a call racing this
  // one reaches the helper and not a freed page.
  writeSlots(fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA),
             Saved, Count);
  if (HMODULE Module = __atomic_exchange_n(Handle, nullptr, __ATOMIC_ACQ_REL))
    FreeLibrary(Module);
  return TRUE;
}

} // extern "C"
