//===-- cxa_thread_atexit.cpp - Thread-local destructor registry ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each thread owns its registry, so registering and draining take no lock.
// A thread's destructors run when it exits, from the image's TLS callback
// under the loader lock, unless something completed the thread earlier; on
// exit, the Universal CRT's thread-exit callback completes the exiting
// thread before any static destructor runs. Process detach runs none: exit
// has run them, or the process ended abruptly and none may run.
//
// A pending destructor keeps its code and its image loaded. The reference is
// taken while the registering image is certainly loaded, and dropped after
// the destructors have returned. Under the loader lock, FreeLibrary cannot
// run, so the thread pool drops those references instead.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

using wincrt::Destructor;

namespace {

struct DtorEntry {
  uintptr_t Dtor; // encoded, null once run
  void *Obj;
  void *Dso;
};

constexpr size_t BlockEntries = 32;

struct DtorBlock {
  DtorEntry Entries[BlockEntries];
  DtorBlock *Next;
  size_t Count;
};

// A reference to an image, which a thread holds while it has a destructor
// in that image or registered by it. The executable, which is never
// unloaded, has a null handle.
struct Module {
  HMODULE Handle;
  uintptr_t Begin;
  uintptr_t Size;
  Module *Next;
};

struct DeferredRelease {
  HMODULE Owner;
  Module *Modules;
};

// Only pointers, so that every thread of the process gets little
// thread-local storage from this image; the blocks come from the heap.
__declspec(thread) DtorBlock *Blocks;
__declspec(thread) Module *Modules;
__declspec(thread) bool Finalizing;

[[noreturn]] void fail() { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }

void *allocate(size_t Size) {
  void *Memory = wincrt::crtAlloc(Size);
  if (!Memory)
    fail();
  return Memory;
}

void releaseModules(Module *List) {
  while (List) {
    Module *Next = List->Next;
    if (List->Handle)
      FreeLibrary(List->Handle);
    wincrt::crtFree(List);
    List = Next;
  }
}

void CALLBACK releaseAfterDetach(PTP_CALLBACK_INSTANCE Instance,
                                 void *Context) {
  auto *Release = static_cast<DeferredRelease *>(Context);
  releaseModules(Release->Modules);
  HMODULE Owner = Release->Owner;
  wincrt::crtFree(Release);
  FreeLibraryWhenCallbackReturns(Instance, Owner);
}

// Keeps the image that contains Address loaded until the thread completes.
// An address outside every image, such as generated code, needs nothing.
void retain(const void *Address) {
  auto Value = reinterpret_cast<uintptr_t>(Address);
  for (Module *M = Modules; M; M = M->Next)
    if (Value - M->Begin < M->Size)
      return;
  HMODULE Handle;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          static_cast<LPCWSTR>(Address), &Handle))
    return;
  const auto *Dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(Handle);
  const auto *Nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
      reinterpret_cast<const char *>(Dos) + Dos->e_lfanew);
  auto *M = static_cast<Module *>(allocate(sizeof(Module)));
  M->Begin = reinterpret_cast<uintptr_t>(Handle);
  M->Size = Nt->OptionalHeader.SizeOfImage;
  M->Next = Modules;
  if (Handle == GetModuleHandleW(nullptr)) {
    FreeLibrary(Handle);
    Handle = nullptr;
  }
  M->Handle = Handle;
  Modules = M;
}

// Runs every entry, newest first. A destructor may register another, which
// then runs too.
void drainAll() {
  while (DtorBlock *Block = Blocks) {
    if (!Block->Count) {
      Blocks = Block->Next;
      wincrt::crtFree(Block);
      continue;
    }
    DtorEntry Entry = Block->Entries[--Block->Count];
    if (Entry.Dtor)
      wincrt::invokeCallback(wincrt::decodePointer(Entry.Dtor), Entry.Obj);
  }
}

// Runs the entries that Dso registered, newest first, holding no block
// pointer across a destructor. The blocks stay until the thread completes.
void drainImage(void *Dso) {
  for (;;) {
    DtorEntry Entry = {};
    for (DtorBlock *Block = Blocks; Block && !Entry.Dtor; Block = Block->Next) {
      for (size_t I = Block->Count; I && !Entry.Dtor; --I) {
        DtorEntry &Candidate = Block->Entries[I - 1];
        if (Candidate.Dtor && Candidate.Dso == Dso) {
          Entry = Candidate;
          Candidate.Dtor = 0;
        }
      }
    }
    if (!Entry.Dtor)
      return;
    wincrt::invokeCallback(wincrt::decodePointer(Entry.Dtor), Entry.Obj);
  }
}

void finalizeThread(bool UnderLoaderLock) {
  if (Finalizing)
    return;
  Finalizing = true;
  drainAll();
  Module *References = Modules;
  Modules = nullptr;
  if (UnderLoaderLock && References) {
    auto *Release =
        static_cast<DeferredRelease *>(allocate(sizeof(DeferredRelease)));
    // The callback's own code must stay loaded until it returns.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&releaseAfterDetach),
                            &Release->Owner))
      fail();
    Release->Modules = References;
    if (!TrySubmitThreadpoolCallback(releaseAfterDetach, Release, nullptr))
      fail();
  } else {
    releaseModules(References);
  }
  Finalizing = false;
}

void NTAPI tlsCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_THREAD_DETACH)
    finalizeThread(true);
}

} // namespace

// Not the first callback of the directory, which is left to the program.
#pragma section(".CRT$XLC", read)
extern "C" __declspec(allocate(".CRT$XLC")) const
    PIMAGE_TLS_CALLBACK WINCRT_LIFETIME(__wincrt_thread_callback) = tlsCallback;

extern "C" {

int WINCRT_LIFETIME(__cxa_thread_atexit_impl)(void (*Function)(void *),
                                              void *Object, void *Dso) {
  if (!Function)
    return -1;
  retain(reinterpret_cast<void *>(Function));
  retain(Dso);
  DtorBlock *Block = Blocks;
  if (!Block || Block->Count == BlockEntries) {
    auto *Fresh = static_cast<DtorBlock *>(allocate(sizeof(DtorBlock)));
    Fresh->Next = Block;
    Blocks = Block = Fresh;
  }
  Block->Entries[Block->Count++] = {wincrt::encodePointer(Function), Object,
                                    Dso};
  return 0;
}

void WINCRT_LIFETIME(__cxa_thread_finalize)(void *Dso) {
  if (Dso)
    drainImage(Dso);
  else
    finalizeThread(false);
}

} // extern "C"
