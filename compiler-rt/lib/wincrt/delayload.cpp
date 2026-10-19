//===-- delayload.cpp - Delay-load helper ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The linker's /delayload thunks call __delayLoadHelper2. It resolves through
// the loader's ResolveDelayLoadedAPI, which knows protected delay-load import
// address tables, API set redirection, the application verifier and the shim
// engine, and which reaches kernel32's failure hook. That hook answers a call
// into a component that is not installed with a substitute stub, so such a
// call returns a failure code instead of crashing.
//
// A failure that nothing substitutes for is reported as delayimp.h specifies:
// the image's failure hook is asked, then a structured exception carrying a
// DelayLoadInfo is raised. The loader's own noncontinuable exception is caught
// here so that only one of the two contracts reaches the program. The failure
// hook's answer is returned but not stored: the loader brackets its own writes
// to a protected table, and a store of this image's could meet the loader
// restoring the protection.
//
// An image that installs the delayimp.h notify hook, which must see every
// stage, or that is linked with /delay:unload, whose library
// __FUnloadDelayLoadedDLL2 must be able to free, resolves in the image
// instead: a library the loader loads for a delayed import becomes a
// dependency of the image, which FreeLibrary does not undo. Both are
// properties of the whole image, so the loader never writes a table that this
// file writes. With a notify or failure hook, each import is resolved when it
// is called, as delayimp.h describes. Without hooks, the first call through a
// descriptor resolves every import that it can, opening a protected table
// once; an import that does not resolve fails only when it is called, as the
// loader's own whole-table resolution leaves it.
//
// The exception is an ABI contract witness, an import the linker adds so that
// a library built with another layout of a type than the image's code uses,
// or linked jointly with the image from other facts, does not bind. Code
// never calls one, so a call through its descriptor first resolves each
// witness that is not yet bound, as a call through it would,
// and a witness that does not resolve fails that call, through the failure
// hook and then the exception, before it reaches the library's code. A
// witness that resolves stays bound, so a later call does not resolve it again.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <delayimp.h>
#include <string.h>

// This file writes a protected table only while it has made the table
// writable, and the loader brackets its own writes, so the linker may keep the
// table read-only whether or not Control Flow Guard is on.
__asm__(".linkprotecteddelayiat");

static_assert(sizeof(ImgDelayDescr) == sizeof(IMAGE_DELAYLOAD_DESCRIPTOR),
              "delayimp.h's descriptor is the SDK's");

extern "C" {
// Exported by kernel32 and forwarded to ntdll; no SDK header declares them.
// The failure hook is kernel32's, whose body holds the substitute stubs.
__declspec(dllimport) PVOID WINAPI ResolveDelayLoadedAPI(
    PVOID ParentModuleBase, const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
    PVOID FailureDllHook, PVOID(WINAPI *FailureSystemHook)(LPCSTR, LPCSTR),
    IMAGE_THUNK_DATA *ThunkAddress, ULONG Flags);
__declspec(dllimport) LONG WINAPI ResolveDelayLoadsFromDll(
    PVOID ParentModuleBase, LPCSTR TargetDllName, ULONG Flags);
__declspec(dllimport) PVOID WINAPI DelayLoadFailureHook(LPCSTR DllName,
                                                        LPCSTR ProcName);

// The null hook of a program that does not define one.
extern const PfnDliHook __wincrt_no_dli_hook = nullptr;

// The section-wide protection choice of a program that does not make one.
// Whatever a program chooses, writeSlots reopens only the pages it writes.
extern const BOOL __wincrt_no_whole_dload_section = FALSE;
}
WINCRT_ALTERNATENAME(__pfnDliNotifyHook2, __wincrt_no_dli_hook)
WINCRT_ALTERNATENAME(__pfnDliFailureHook2, __wincrt_no_dli_hook)
WINCRT_ALTERNATENAME(__bChangeProtectionOfWholeDloadSection,
                     __wincrt_no_whole_dload_section)

namespace {

// The loader refuses a resolution whose flags name anything but a search
// path, so only those are passed on: the DLL load directory, the application
// directory, System32, the default directories, the safe current directories,
// and System32 without forwarders. The user directories need a process-wide
// policy this image cannot see, and are left out rather than risk refusal.
constexpr DWORD SearchPathFlags = 0x7b00;

// The number of slots resolved in the image under one opening of the table.
constexpr size_t SlotsPerWrite = 64;

// The search-path restriction the image asks for through /dependentloadflag.
// The loader applies it to the eager imports only.
DWORD searchPathFlags() {
  return static_cast<const volatile LoadConfig &>(_load_config_used)
             .DependentLoadFlags &
         SearchPathFlags;
}

bool delayIatIsProtected() {
  return static_cast<const volatile LoadConfig &>(_load_config_used)
             .GuardFlagsAndCodeIntegrity &
         IMAGE_GUARD_PROTECT_DELAYLOAD_IAT;
}

template <typename T> T *fromRva(DWORD Rva) {
  return reinterpret_cast<T *>(reinterpret_cast<char *>(&__ImageBase) + Rva);
}

const IMAGE_NT_HEADERS *ntHeaders() {
  return fromRva<const IMAGE_NT_HEADERS>(
      static_cast<DWORD>(__ImageBase.e_lfanew));
}

// Other threads call through a slot while it is resolved, so a slot is read
// and written atomically. Calls need no ordering beyond the slot's own.
ULONG_PTR loadSlot(const IMAGE_THUNK_DATA &Slot) {
  return __atomic_load_n(&Slot.u1.Function, __ATOMIC_RELAXED);
}

// Whether a slot still holds its initial value, the address of the linker's
// thunk in this image, as the loader decides it.
bool isUnbound(const IMAGE_THUNK_DATA &Slot) {
  return loadSlot(Slot) - reinterpret_cast<ULONG_PTR>(&__ImageBase) <
         ntHeaders()->OptionalHeader.SizeOfImage;
}

// A protected table is read-only except while it is written. Two threads may
// write slots on one page, and one must not restore the protection while the
// other is still storing, so the image serializes its writes.
SRWLOCK TableLock = SRWLOCK_INIT;

// Writes the nonzero entries of Values into Count slots.
void writeSlots(IMAGE_THUNK_DATA *Slots, const IMAGE_THUNK_DATA *Values,
                size_t Count) {
  size_t Written = 0;
  for (size_t I = 0; I != Count; ++I)
    Written += Values[I].u1.Function != 0;
  if (!Written)
    return;
  const bool Protected = delayIatIsProtected();
  const SIZE_T Bytes = Count * sizeof(*Slots);
  DWORD Old;
  if (Protected) {
    AcquireSRWLockExclusive(&TableLock);
    if (!VirtualProtect(Slots, Bytes, PAGE_READWRITE, &Old)) {
      ReleaseSRWLockExclusive(&TableLock);
      return;
    }
  }
  for (size_t I = 0; I != Count; ++I)
    if (Values[I].u1.Function)
      __atomic_store_n(&Slots[I].u1.Function, Values[I].u1.Function,
                       __ATOMIC_RELAXED);
  if (Protected) {
    VirtualProtect(Slots, Bytes, PAGE_READONLY, &Old);
    ReleaseSRWLockExclusive(&TableLock);
  }
}

// The descriptor's two tables are parallel, so a slot's index in the import
// address table is its name's index in the name table.
DelayLoadInfo describeImport(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                             IMAGE_THUNK_DATA *Slot) {
  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Names = fromRva<const IMAGE_THUNK_DATA>(Descriptor->ImportNameTableRVA);
  const IMAGE_THUNK_DATA &Name = Names[Slot - Iat];

  DelayLoadInfo Dli = {};
  Dli.cb = sizeof(Dli);
  Dli.pidd = reinterpret_cast<PCImgDelayDescr>(Descriptor);
  Dli.ppfn = reinterpret_cast<FARPROC *>(&Slot->u1.Function);
  Dli.szDll = fromRva<const char>(Descriptor->DllNameRVA);
  if (IMAGE_SNAP_BY_ORDINAL(Name.u1.Ordinal)) {
    Dli.dlp.dwOrdinal = static_cast<DWORD>(IMAGE_ORDINAL(Name.u1.Ordinal));
  } else {
    Dli.dlp.fImportByName = TRUE;
    Dli.dlp.szProcName =
        fromRva<IMAGE_IMPORT_BY_NAME>(static_cast<DWORD>(Name.u1.AddressOfData))
            ->Name;
  }
  return Dli;
}

LPCSTR procNameOrOrdinal(const DelayLoadInfo &Dli) {
  return Dli.dlp.fImportByName ? Dli.dlp.szProcName
                               : reinterpret_cast<LPCSTR>(
                                     static_cast<ULONG_PTR>(Dli.dlp.dwOrdinal));
}

FARPROC notify(unsigned Reason, DelayLoadInfo &Dli) {
  return __pfnDliNotifyHook2 ? __pfnDliNotifyHook2(Reason, &Dli) : nullptr;
}

// The calling thunk has nowhere to put an error, so a failure that no hook
// answers becomes an exception. A filter may resume the raise, and the helper
// then returns null, as delayimp.h documents.
FARPROC raiseFailure(DWORD Error, DelayLoadInfo &Dli) {
  ULONG_PTR Arguments[] = {reinterpret_cast<ULONG_PTR>(&Dli)};
  SetLastError(Dli.dwLastError);
  RaiseException(VcppException(ERROR_SEVERITY_ERROR, Error), 0, 1, Arguments);
  return nullptr;
}

// The library could not be loaded. A failure hook that answers returns a
// library it loaded itself.
HMODULE failLoadLibrary(DelayLoadInfo &Dli) {
  if (__pfnDliFailureHook2)
    if (auto Module = reinterpret_cast<HMODULE>(
            __pfnDliFailureHook2(dliFailLoadLib, &Dli)))
      return Module;
  raiseFailure(ERROR_MOD_NOT_FOUND, Dli);
  return nullptr;
}

bool isWitnessName(const char *Proc);

// The library has no such procedure. A failure hook that answers returns a
// substitute, except for a witness: what it proves, the library's layout, no
// substitute can.
FARPROC failGetProcAddress(DelayLoadInfo &Dli) {
  if (__pfnDliFailureHook2)
    if (FARPROC Proc = __pfnDliFailureHook2(dliFailGetProc, &Dli))
      if (!Dli.dlp.fImportByName || !isWitnessName(Dli.dlp.szProcName))
        return Proc;
  return raiseFailure(ERROR_PROC_NOT_FOUND, Dli);
}

// Everything the loader tried has failed. It reports one reason for both
// stages, so they are told apart by whether the library is loaded.
FARPROC reportLoaderFailure(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                            IMAGE_THUNK_DATA *Slot) {
  DelayLoadInfo Dli = describeImport(Descriptor, Slot);
  Dli.dwLastError = GetLastError();
  HMODULE Module = *fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  if (!Module)
    Module = GetModuleHandleA(Dli.szDll);
  if (!Module) {
    Module = failLoadLibrary(Dli);
    if (!Module)
      return nullptr;
    Dli.hmodCur = Module;
    if (FARPROC Proc = GetProcAddress(Module, procNameOrOrdinal(Dli)))
      return Proc;
    Dli.dwLastError = GetLastError();
  }
  Dli.hmodCur = Module;
  return failGetProcAddress(Dli);
}

// Loads the descriptor's library in the image, keeping one handle in the
// descriptor's slot. When Report is set, a failure goes to the failure hook
// and then becomes an exception.
HMODULE loadInImage(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                    DelayLoadInfo &Dli, bool Report) {
  auto *ModuleSlot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  if (HMODULE Module = __atomic_load_n(ModuleSlot, __ATOMIC_ACQUIRE))
    return Module;
  auto Module = reinterpret_cast<HMODULE>(notify(dliNotePreLoadLibrary, Dli));
  if (!Module)
    Module = LoadLibraryExA(Dli.szDll, nullptr, searchPathFlags());
  if (!Module && Report) {
    Dli.dwLastError = GetLastError();
    Module = failLoadLibrary(Dli);
  }
  if (!Module)
    return nullptr;
  // Another thread may have loaded the library first.
  HMODULE Expected = nullptr;
  if (!__atomic_compare_exchange_n(ModuleSlot, &Expected, Module, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    FreeLibrary(Module);
    Module = Expected;
  }
  return Module;
}

// Resolves one import in the image, telling the hooks of each stage as
// delayimp.h describes. An answer to dliStartProcessing is returned without
// being stored.
FARPROC resolveImport(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                      IMAGE_THUNK_DATA *Slot) {
  DelayLoadInfo Dli = describeImport(Descriptor, Slot);
  FARPROC Proc = notify(dliStartProcessing, Dli);
  if (!Proc) {
    HMODULE Module = loadInImage(Descriptor, Dli, /*Report=*/true);
    if (!Module)
      return nullptr;
    Dli.hmodCur = Module;
    Proc = notify(dliNotePreGetProcAddress, Dli);
    if (!Proc)
      Proc = GetProcAddress(Module, procNameOrOrdinal(Dli));
    if (!Proc) {
      Dli.dwLastError = GetLastError();
      Proc = failGetProcAddress(Dli);
      if (!Proc)
        return nullptr;
    }
    IMAGE_THUNK_DATA Value;
    Value.u1.Function = reinterpret_cast<ULONG_PTR>(Proc);
    writeSlots(Slot, &Value, 1);
  }
  Dli.pfnCur = Proc;
  notify(dliNoteEndProcessing, Dli);
  return Proc;
}

// Resolves every slot of the descriptor that still holds its thunk, in an
// image with no hooks. Only a failure of Requested is reported: another slot
// that does not resolve keeps its thunk, so that it resolves or fails when it
// is called, as the loader leaves it. Returns Requested's new value, and sets
// Error when some slot did not resolve.
FARPROC resolveDescriptor(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                          const IMAGE_THUNK_DATA *Requested, DWORD &Error) {
  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Names = fromRva<const IMAGE_THUNK_DATA>(Descriptor->ImportNameTableRVA);
  HMODULE Module = nullptr;
  bool TriedLoad = false;
  FARPROC Result = nullptr;
  Error = ERROR_SUCCESS;
  for (size_t First = 0; Names[First].u1.AddressOfData;) {
    IMAGE_THUNK_DATA Values[SlotsPerWrite] = {};
    size_t Count = 0;
    for (; Count != SlotsPerWrite && Names[First + Count].u1.AddressOfData;
         ++Count) {
      IMAGE_THUNK_DATA *Slot = &Iat[First + Count];
      const bool Wanted = Slot == Requested;
      // Another thread may have resolved the slot already.
      if (!isUnbound(*Slot)) {
        if (Wanted)
          Result = reinterpret_cast<FARPROC>(loadSlot(*Slot));
        continue;
      }
      DelayLoadInfo Dli = describeImport(Descriptor, Slot);
      // A library that did not load is tried again only to report it.
      if (!Module && (!TriedLoad || Wanted)) {
        Module = loadInImage(Descriptor, Dli, Wanted);
        TriedLoad = true;
      }
      FARPROC Proc = nullptr;
      if (Module) {
        Proc = GetProcAddress(Module, procNameOrOrdinal(Dli));
        if (!Proc && Wanted) {
          Dli.hmodCur = Module;
          Dli.dwLastError = GetLastError();
          Proc = failGetProcAddress(Dli);
        }
      }
      if (Proc)
        Values[Count].u1.Function = reinterpret_cast<ULONG_PTR>(Proc);
      else if (Error == ERROR_SUCCESS)
        Error = Module ? ERROR_PROC_NOT_FOUND : ERROR_MOD_NOT_FOUND;
      if (Wanted)
        Result = Proc;
    }
    writeSlots(&Iat[First], Values, Count);
    First += Count;
  }
  return Result;
}

// Whether a name table entry is that of an ABI contract witness, which names
// a type's or datum's layout and its digest: a standalone witness, the
// type_info object of a type that the library owns, under the name that the
// type's layout qualifies, or the identity of a library linked jointly with
// the image.
bool isWitnessName(const char *Proc) {
  return !strncmp(Proc, "__abiw$", 7) || !strncmp(Proc, "__abis$", 7) ||
         !strncmp(Proc, "__llvm_private$", 15) ||
         (!strncmp(Proc, "_ZTI", 4) && strstr(Proc, "$abi$"));
}

bool isWitness(const IMAGE_THUNK_DATA &Name) {
  if (IMAGE_SNAP_BY_ORDINAL(Name.u1.Ordinal))
    return false;
  return isWitnessName(
      fromRva<IMAGE_IMPORT_BY_NAME>(static_cast<DWORD>(Name.u1.AddressOfData))
          ->Name);
}

bool hasHooks() { return __pfnDliNotifyHook2 || __pfnDliFailureHook2; }

bool resolvesInImage(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor) {
  return __pfnDliNotifyHook2 || Descriptor->UnloadInformationTableRVA;
}

// This image's descriptor for a library, by the name the import library
// wrote, compared without regard to case as the loader compares it.
const IMAGE_DELAYLOAD_DESCRIPTOR *findDescriptor(LPCSTR DllName) {
  DWORD Rva =
      ntHeaders()
          ->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
          .VirtualAddress;
  if (!Rva)
    return nullptr;
  for (auto *D = fromRva<const IMAGE_DELAYLOAD_DESCRIPTOR>(Rva); D->DllNameRVA;
       ++D)
    if (!_stricmp(fromRva<const char>(D->DllNameRVA), DllName))
      return D;
  return nullptr;
}

bool resolveWitnesses(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                      const IMAGE_THUNK_DATA *Slot);

} // namespace

extern "C" {

FARPROC WINAPI __delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                                  FARPROC *Entry) {
  auto *Slot = reinterpret_cast<IMAGE_THUNK_DATA *>(Entry);
  if (!Descriptor->Attributes.RvaBased) {
    DelayLoadInfo Dli = {};
    Dli.cb = sizeof(Dli);
    Dli.pidd = reinterpret_cast<PCImgDelayDescr>(Descriptor);
    Dli.ppfn = Entry;
    return raiseFailure(ERROR_INVALID_PARAMETER, Dli);
  }
  if (!resolveWitnesses(Descriptor, Slot))
    return nullptr;

  if (resolvesInImage(Descriptor)) {
    if (hasHooks())
      return resolveImport(Descriptor, Slot);
    DWORD Error;
    return resolveDescriptor(Descriptor, Slot, Error);
  }

  FARPROC Proc = nullptr;
  __try {
    Proc = reinterpret_cast<FARPROC>(
        ResolveDelayLoadedAPI(&__ImageBase, Descriptor, nullptr,
                              DelayLoadFailureHook, Slot, searchPathFlags()));
  } __except (GetExceptionCode() == ERROR_DELAY_LOAD_FAILED
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    Proc = nullptr;
  }
  return Proc ? Proc : reportLoaderFailure(Descriptor, Slot);
}

// Restores the address table this image started with and frees the library,
// so that a later call loads it again. FALSE if the image does not delay-load
// the library, has not loaded it, or was linked without /delay:unload and so
// has no copy of the table to restore.
BOOL WINAPI __FUnloadDelayLoadedDLL2(LPCSTR DllName) {
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor = findDescriptor(DllName);
  if (!Descriptor || !Descriptor->UnloadInformationTableRVA)
    return FALSE;
  auto *ModuleSlot = fromRva<HMODULE>(Descriptor->ModuleHandleRVA);
  if (!__atomic_load_n(ModuleSlot, __ATOMIC_ACQUIRE))
    return FALSE;

  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Saved =
      fromRva<const IMAGE_THUNK_DATA>(Descriptor->UnloadInformationTableRVA);
  size_t Count = 0;
  while (Saved[Count].u1.Function)
    ++Count;
  // The thunks go back before the library does, so that a call racing this
  // one reaches the helper rather than a freed page.
  writeSlots(Iat, Saved, Count);
  if (HMODULE Module =
          __atomic_exchange_n(ModuleSlot, nullptr, __ATOMIC_ACQ_REL))
    FreeLibrary(Module);
  return TRUE;
}

// Resolves every delayed import of one library now. A failure is reported as
// a call reports it: to the failure hook, and otherwise as the exception the
// helper raises, so that one handler serves every import, as Microsoft's
// documentation of delayimp.h describes. The result is an error only for a
// library the image does not delay-load, or a failure that a filter resumed.
HRESULT WINAPI __HrLoadAllImportsForDll(LPCSTR DllName) {
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor = findDescriptor(DllName);
  if (!Descriptor)
    return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  if (resolvesInImage(Descriptor)) {
    if (hasHooks()) {
      for (IMAGE_THUNK_DATA *Slot = Iat; loadSlot(*Slot); ++Slot)
        if (isUnbound(*Slot))
          resolveImport(Descriptor, Slot);
      return S_OK;
    }
    DWORD Error;
    resolveDescriptor(Descriptor, nullptr, Error);
    for (IMAGE_THUNK_DATA *Slot = Iat; Error && loadSlot(*Slot); ++Slot)
      if (isUnbound(*Slot))
        resolveDescriptor(Descriptor, Slot, Error);
    return HRESULT_FROM_WIN32(Error);
  }
  if (ResolveDelayLoadsFromDll(&__ImageBase, DllName, 0) >= 0)
    return S_OK;
  for (IMAGE_THUNK_DATA *Slot = Iat; loadSlot(*Slot); ++Slot)
    if (isUnbound(*Slot) && !reportLoaderFailure(Descriptor, Slot))
      return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  return S_OK;
}

} // extern "C"

namespace {

// Resolves each ABI contract witness of the descriptor that is not yet bound,
// unless Slot is itself one, as a call through it would. Returns false when
// one did not resolve, after its failure was reported.
bool resolveWitnesses(const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor,
                      const IMAGE_THUNK_DATA *Slot) {
  auto *Iat = fromRva<IMAGE_THUNK_DATA>(Descriptor->ImportAddressTableRVA);
  auto *Names = fromRva<const IMAGE_THUNK_DATA>(Descriptor->ImportNameTableRVA);
  if (isWitness(Names[Slot - Iat]))
    return true;
  for (size_t I = 0; Names[I].u1.AddressOfData; ++I)
    if (isUnbound(Iat[I]) && isWitness(Names[I]) &&
        !__delayLoadHelper2(Descriptor,
                            reinterpret_cast<FARPROC *>(&Iat[I].u1.Function)))
      return false;
  return true;
}

} // namespace
