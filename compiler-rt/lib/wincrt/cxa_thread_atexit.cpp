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
// A thread's registry is the process's, in one TLS slot that every copy of
// the registries shares (exit_state.h), so that its destructors run in exact
// reverse order whichever copy registered them, and the first copy's TLS
// callback to see the thread exit runs them all. A thread that never
// registers one has none, and no image needs static thread-local storage for
// it.
//
// A pending destructor keeps its code and its image loaded. The reference is
// taken while the registering image is certainly loaded, and dropped after
// the destructors have returned. Under the loader lock, FreeLibrary cannot
// run, so the thread pool drops those references instead.
//
//===----------------------------------------------------------------------===//

#include "exit_state.h"

using namespace wincrt;

namespace {

using DtorEntry = ThreadExitEntry;
using DtorBlock = ThreadExitBlock;
using Module = ThreadExitModule;
constexpr uint32_t BlockEntries = ThreadExitBlockEntries;

static_assert(BlockEntries <= 32, "DtorBlock::Salted has a bit per entry");

struct DeferredRelease {
  HMODULE Owner;
  Module *Modules;
};

// A registration that cannot allocate fails, as __cxa_atexit does. A thread's
// completion has no caller to report to, so it fails fast instead.
[[noreturn]] void fail() { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }

void releaseModules(Module *List) {
  while (List) {
    Module *Next = List->Next;
    if (List->Handle)
      FreeLibrary(List->Handle);
    crtFree(List);
    List = Next;
  }
}

void CALLBACK releaseAfterDetach(PTP_CALLBACK_INSTANCE Instance,
                                 void *Context) {
  auto *Release = static_cast<DeferredRelease *>(Context);
  releaseModules(Release->Modules);
  HMODULE Owner = Release->Owner;
  crtFree(Release);
  FreeLibraryWhenCallbackReturns(Instance, Owner);
}

// Keeps the image that contains Address loaded until the thread completes.
// An address outside every image, such as generated code, needs nothing.
// Returns false if it cannot.
bool retain(ThreadExitState &T, const void *Address) {
  auto Value = reinterpret_cast<uintptr_t>(Address);
  for (Module *M = T.Modules; M; M = M->Next)
    if (Value - M->Begin < M->Size)
      return true;
  HMODULE Handle;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          static_cast<LPCWSTR>(Address), &Handle))
    return true;
  auto *M = static_cast<Module *>(crtAlloc(sizeof(Module)));
  if (!M) {
    FreeLibrary(Handle);
    return false;
  }
  M->Begin = reinterpret_cast<uintptr_t>(Handle);
  M->Size = imageSize(Handle);
  M->Next = T.Modules;
  if (Handle == GetModuleHandleW(nullptr)) {
    FreeLibrary(Handle);
    Handle = nullptr;
  }
  M->Handle = Handle;
  T.Modules = M;
  return true;
}

// Runs every entry, newest first. A destructor may register another, which
// then runs too.
void drainAll(ExitState &S, ThreadExitState &T) {
  while (DtorBlock *Block = T.Blocks) {
    if (!Block->Count) {
      T.Blocks = Block->Next;
      crtFree(Block);
      continue;
    }
    uint32_t I = --Block->Count;
    DtorEntry Entry = Block->Entries[I];
    if (Entry.Dtor)
      invokeCallback(decodeExit(S, Entry.Dtor), Entry.Obj,
                     Block->Salted >> I & 1);
  }
}

// Runs the entries that Dso registered, newest first, holding no block
// pointer across a destructor. The blocks stay until the thread completes.
void drainImage(ExitState &S, ThreadExitState &T, void *Dso) {
  for (;;) {
    DtorEntry Entry = {};
    bool Salted = false;
    for (DtorBlock *Block = T.Blocks; Block && !Entry.Dtor;
         Block = Block->Next) {
      for (uint32_t I = Block->Count; I && !Entry.Dtor; --I) {
        DtorEntry &Candidate = Block->Entries[I - 1];
        if (Candidate.Dtor && Candidate.Dso == Dso) {
          Entry = Candidate;
          Salted = Block->Salted >> (I - 1) & 1;
          Candidate.Dtor = 0;
        }
      }
    }
    if (!Entry.Dtor)
      return;
    invokeCallback(decodeExit(S, Entry.Dtor), Entry.Obj, Salted);
  }
}

void finalizeThread(bool UnderLoaderLock) {
  ExitState &S = exitState();
  ThreadExitState *State = threadExitState(/*Create=*/false);
  if (!State || State->Finalizing)
    return;
  ThreadExitState &T = *State;
  T.Finalizing = true;
  drainAll(S, T);
  Module *References = T.Modules;
  releaseThreadExitState(State);
  if (UnderLoaderLock && References) {
    auto *Release =
        static_cast<DeferredRelease *>(crtAlloc(sizeof(DeferredRelease)));
    // The callback's own code must stay loaded until it returns.
    if (!Release ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&releaseAfterDetach),
                            &Release->Owner))
      fail();
    Release->Modules = References;
    if (!TrySubmitThreadpoolCallback(releaseAfterDetach, Release, nullptr))
      fail();
  } else {
    releaseModules(References);
  }
}

void NTAPI tlsCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_THREAD_DETACH)
    finalizeThread(true);
}

int append(Destructor Function, void *Object, void *Dso, bool Salted) {
  ExitState &S = exitState();
  ThreadExitState *State = threadExitState(/*Create=*/true);
  if (!State)
    return -1;
  ThreadExitState &T = *State;
  if (!Function || !retain(T, reinterpret_cast<void *>(Function)) ||
      !retain(T, Dso))
    return -1;
  DtorBlock *Block = T.Blocks;
  if (!Block || Block->Count == BlockEntries) {
    auto *Fresh = static_cast<DtorBlock *>(crtAlloc(sizeof(DtorBlock)));
    if (!Fresh)
      return -1;
    Fresh->Next = Block;
    Fresh->Count = 0;
    Fresh->Salted = 0;
    T.Blocks = Block = Fresh;
  }
  // A slot that drainAll emptied can be filled again, so the bit is written
  // either way.
  uint32_t Bit = uint32_t(1) << Block->Count;
  Block->Salted = Salted ? Block->Salted | Bit : Block->Salted & ~Bit;
  Block->Entries[Block->Count++] = {encodeExit(S, Function), Object, Dso};
  return 0;
}

} // namespace

// Not the first callback of the directory, which is left to the program. The
// directory is linked with it, since the registry has no thread-local data to
// name it.
WINCRT_INCLUDE(_tls_used)
#pragma section(".CRT$XLC", read)
extern "C" __declspec(allocate(".CRT$XLC"))
const PIMAGE_TLS_CALLBACK __wincrt_thread_callback = tlsCallback;

extern "C" {

int __cxa_thread_atexit_impl(void (*Function)(void *), void *Object,
                             void *Dso) {
  return append(Function, Object, Dso, false);
}

// The C++ runtime's __llvm_kcfi_cxa_thread_atexit, which clang registers its
// destructors with, since they carry the salted type.
int __llvm_kcfi_cxa_thread_atexit_impl(void (*Function)(void *), void *Object,
                                       void *Dso) {
  return append(Function, Object, Dso, true);
}

void __cxa_thread_finalize(void *Dso) {
  if (!Dso)
    finalizeThread(false);
  else if (ThreadExitState *T = threadExitState(/*Create=*/false))
    drainImage(exitState(), *T, Dso);
}

} // extern "C"
