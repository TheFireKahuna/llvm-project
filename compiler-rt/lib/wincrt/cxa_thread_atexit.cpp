//===-- cxa_thread_atexit.cpp - Thread-local destructor registry ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each OS thread owns its registry, so registration and draining need no
// lock. The shared C++ runtime drains a thread explicitly before its thread
// procedure returns; threads that bypass libc++ are drained by the PE TLS
// callback on thread detach, under the loader lock. Process detach never
// drains: normal exit drains the exiting thread through the UCRT callback
// first, and abrupt termination must not run destructors.
//
// A pending destructor keeps its code and its DSO image loaded: the reference
// is taken while the registering image is certainly alive, and released only
// after the callbacks have returned into still-mapped code. When the release
// happens under the loader lock it is posted to the thread pool, because
// FreeLibrary cannot take that lock re-entrantly.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#ifdef WINCRT_SHARED_CXX_RUNTIME
#include <__thread/thread.h>
#define WINCRT_TLS_CALLBACK __wincrt_shared_tls_callback
#else
#define WINCRT_TLS_CALLBACK __wincrt_local_tls_callback
#endif

namespace {

using wincrt::DtorBlock;
using wincrt::DtorEntry;

struct Module {
  HMODULE Handle;
  uintptr_t End;
  Module *Next;
};

struct DeferredRelease {
  HMODULE Owner;
  Module *Modules;
};

__declspec(thread) DtorBlock FirstBlock;
__declspec(thread) DtorBlock *CurrentBlock;
__declspec(thread) Module *Modules;
__declspec(thread) bool Finalizing;
#ifdef WINCRT_SHARED_CXX_RUNTIME
__declspec(thread) std::__thread_struct *LibraryData;
__declspec(thread) bool LibraryOwnerRetained;
#endif

[[noreturn]] void fail() { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }

void *allocate(size_t Size) {
  void *Memory = wincrt::crtAlloc(Size);
  if (!Memory)
    fail();
  return Memory;
}

HMODULE reference(const void *Address) {
  HMODULE Handle;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          static_cast<LPCWSTR>(Address), &Handle))
    fail();
  return Handle;
}

void releaseModules(Module *List) {
  while (List) {
    Module *Next = List->Next;
    FreeLibrary(List->Handle);
    wincrt::crtFree(List);
    List = Next;
  }
}

void CALLBACK releaseAfterDetach(PTP_CALLBACK_INSTANCE Instance,
                                 void *Context) {
  auto *Release = static_cast<DeferredRelease *>(Context);
  releaseModules(Release->Modules);
  FreeLibraryWhenCallbackReturns(Instance, Release->Owner);
  wincrt::crtFree(Release);
}

// Keeps the image containing Address loaded until this thread's registry is
// drained. References are coalesced per image; the executable needs none.
void retain(const void *Address) {
  if (!Address)
    return;
  auto Value = reinterpret_cast<uintptr_t>(Address);
  for (Module *M = Modules; M; M = M->Next)
    if (Value >= reinterpret_cast<uintptr_t>(M->Handle) && Value < M->End)
      return;
  HMODULE Handle = reference(Address);
  if (Handle == GetModuleHandleW(nullptr))
    return;
  const auto *Base = reinterpret_cast<const unsigned char *>(Handle);
  const auto *Headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(
      Base + reinterpret_cast<const IMAGE_DOS_HEADER *>(Base)->e_lfanew);
  auto *M = static_cast<Module *>(allocate(sizeof(Module)));
  *M = {Handle,
        reinterpret_cast<uintptr_t>(Handle) +
            Headers->OptionalHeader.SizeOfImage,
        Modules};
  Modules = M;
}

void invoke(void (*Function)(void *), void *Object) {
  wincrt::invokeCallback(Function, Object);
}

// Pops and runs every entry. Callbacks may register more; those run too.
void drainAll() {
  while (DtorBlock *Block = CurrentBlock) {
    if (!Block->Count) {
      CurrentBlock = Block->Next;
      if (Block != &FirstBlock)
        wincrt::crtFree(Block);
      continue;
    }
    DtorEntry Entry = Block->Entries[--Block->Count];
    if (Entry.Dtor)
      invoke(Entry.Dtor, Entry.Obj);
  }
}

// Runs the entries registered by Dso, most recent first, without holding a
// block pointer across a callback. Blocks are not freed here: the image's
// references stay in place until the thread's own drain.
void drainImage(void *Dso) {
  for (;;) {
    DtorEntry Entry{};
    for (DtorBlock *Block = CurrentBlock; Block && !Entry.Dtor;
         Block = Block->Next)
      for (size_t I = Block->Count; I && !Entry.Dtor; --I)
        if (Block->Entries[I - 1].Dtor && Block->Entries[I - 1].Dso == Dso) {
          Entry = Block->Entries[I - 1];
          Block->Entries[I - 1].Dtor = nullptr;
        }
    if (!Entry.Dtor)
      return;
    invoke(Entry.Dtor, Entry.Obj);
  }
}

// Completes the calling thread: language TLS first, then the shared
// runtime's thread state, then the image references.
void finalizeThread(bool UnderLoaderLock) {
  if (Finalizing)
    return;
  Finalizing = true;
  drainAll();
#ifdef WINCRT_SHARED_CXX_RUNTIME
  std::__thread_struct *Data = LibraryData;
  LibraryData = nullptr;
  delete Data;
  LibraryOwnerRetained = false;
#endif
  Module *Refs = Modules;
  Modules = nullptr;
  if (UnderLoaderLock && Refs) {
    auto *Release =
        static_cast<DeferredRelease *>(allocate(sizeof(DeferredRelease)));
    Release->Owner = reference(reinterpret_cast<void *>(&releaseAfterDetach));
    Release->Modules = Refs;
    if (!TrySubmitThreadpoolCallback(releaseAfterDetach, Release, nullptr))
      fail();
  } else {
    releaseModules(Refs);
  }
  Finalizing = false;
}

void NTAPI tlsCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_THREAD_DETACH)
    finalizeThread(true);
}

} // namespace

#pragma section(".CRT$XLC", long, read)
extern "C"
    __declspec(allocate(".CRT$XLC")) PIMAGE_TLS_CALLBACK WINCRT_TLS_CALLBACK =
        tlsCallback;

extern "C" {

int WINCRT_LIFETIME(__cxa_thread_atexit_impl)(void (*Function)(void *),
                                              void *Object, void *Dso) {
  if (!Function)
    return -1;
  retain(reinterpret_cast<void *>(Function));
  retain(Dso);
  if (!CurrentBlock)
    CurrentBlock = &FirstBlock;
  if (!CurrentBlock->hasSpace()) {
    auto *Block = static_cast<DtorBlock *>(allocate(sizeof(DtorBlock)));
    Block->Next = CurrentBlock;
    CurrentBlock = Block;
  }
  CurrentBlock->push(Function, Object, Dso);
  return 0;
}

void WINCRT_LIFETIME(__cxa_thread_finalize)(void *Dso) {
  if (Dso)
    drainImage(Dso);
  else
    finalizeThread(false);
}

} // extern "C"

#ifdef WINCRT_SHARED_CXX_RUNTIME
_LIBCPP_BEGIN_NAMESPACE_STD
__thread_struct *&__thread_local_data_ref() {
  if (!LibraryOwnerRetained) {
    retain(reinterpret_cast<void *>(&__thread_local_data_ref));
    LibraryOwnerRetained = true;
  }
  return LibraryData;
}
_LIBCPP_END_NAMESPACE_STD
#endif
