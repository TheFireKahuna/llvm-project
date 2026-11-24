//===-- cxa_thread_atexit.cpp - Thread-local destructors ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each thread owns its list, so registering and draining take no lock. A
// thread's destructors run when it exits, from the image's TLS callback under
// the loader lock, unless something completed the thread earlier; on exit, the
// Universal CRT's thread-exit callback completes the exiting thread before any
// static destructor runs. Process detach runs none: exit has run them, or the
// process ended abruptly and none may run.
//
// A thread's list is in its block's header, which the process's one
// thread-local runtime keeps (tlsrt.h), so that its destructors run in exact
// reverse order whichever image registered them. A thread that never
// registers one has none.
//
// A pending destructor keeps its code and its image loaded. The reference is
// taken while the registering image is certainly loaded, and dropped after the
// destructors have returned.
//
//===----------------------------------------------------------------------===//

#include "tlsrt.h"

using namespace wincrt;
using namespace wincrt::tls;

struct wincrt::tls::Dtor {
  Dtor *Next;
  uintptr_t Function; // encoded with the thread-local runtime's cookie
  void *Obj;
  void *Dso;
  // Whether Function was registered as carrying the salted destructor type.
  bool Salted;
};

struct wincrt::tls::Module {
  Module *Next;
  HMODULE Handle; // null for the executable, which is never unloaded
  uintptr_t Begin;
  uintptr_t Size;
};

namespace {

// The calling thread's state, in its block, which it is adopted for if
// Create is set; null if it has none, or none can be made.
ThreadState *threadState(bool Create) {
  Header *H = header(Create);
  return H ? &H->Thread : nullptr;
}

// Keeps the image that contains Address loaded until the thread completes.
// An address outside every image, such as generated code, needs nothing.
// Returns false if it cannot.
bool retain(ThreadState &T, void *Address) {
  for (Module *M = T.Modules; M; M = M->Next)
    if (reinterpret_cast<uintptr_t>(Address) - M->Begin < M->Size)
      return true;
  HMODULE Handle;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          static_cast<LPCWSTR>(Address), &Handle))
    return true;
  bool Executable = Handle == GetModuleHandleW(nullptr);
  auto *M = static_cast<Module *>(crtAlloc(sizeof(Module)));
  if (!M || Executable)
    FreeLibrary(Handle);
  if (!M)
    return false;
  *M = {T.Modules, Executable ? nullptr : Handle,
        reinterpret_cast<uintptr_t>(Handle), imageSize(Handle)};
  T.Modules = M;
  return true;
}

void releaseModules(Module *List) {
  while (Module *M = List) {
    List = M->Next;
    if (M->Handle)
      FreeLibrary(M->Handle);
    crtFree(M);
  }
}

// FreeLibrary cannot run under the loader lock, so the thread pool drops a
// completed thread's references there, and then the one that kept this code
// loaded for it.
void CALLBACK releaseAfterDetach(PTP_CALLBACK_INSTANCE Instance, void *List) {
  releaseModules(static_cast<Module *>(List));
  HMODULE Self;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&releaseAfterDetach), &Self))
    FreeLibraryWhenCallbackReturns(Instance, Self);
}

// Runs the destructors that Dso registered, or all of them if Dso is null,
// newest first. A destructor may register another, which then runs next.
void drain(ThreadState &T, void *Dso) {
  for (Dtor **Link = &T.Dtors; Dtor *D = *Link;) {
    if (Dso && D->Dso != Dso) {
      Link = &D->Next;
      continue;
    }
    *Link = D->Next;
    Dtor Entry = *D;
    crtFree(D);
    invokeCallback(decodeExit(shared().Cookie, Entry.Function), Entry.Obj,
                   Entry.Salted);
    Link = &T.Dtors;
  }
}

// Drops a list of modules from under the loader lock: FreeLibrary cannot run
// under it.
void releaseLater(Module *Modules) {
  HMODULE Self;
  // A thread's completion has no caller to report a failure to.
  if (Modules &&
      (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&releaseAfterDetach),
                           &Self) ||
       !TrySubmitThreadpoolCallback(releaseAfterDetach, Modules, nullptr)))
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

void finalizeThread(bool UnderLoaderLock) {
  ThreadState *T = threadState(/*Create=*/false);
  if (!T)
    return;
  // A destructor that calls exit continues the list; only the first call
  // completes the thread.
  if (T->Finalizing)
    return drain(*T, nullptr);
  T->Finalizing = true;
  drain(*T, nullptr);
  Module *Modules = T->Modules;
  *T = {};
  if (UnderLoaderLock)
    releaseLater(Modules);
  else
    releaseModules(Modules);
}

// The release at the thread's last hook: this callback, or the executable's
// last, which holds __wincrt_thread_end where the executable has callbacks of
// its own, so that they still read their thread-locals.
bool TailHeld;

WINCRT_NO_TP void NTAPI threadCallback(void *, DWORD Reason, void *) {
  if (Reason != DLL_THREAD_DETACH)
    return;
  finalizeThread(/*UnderLoaderLock=*/true);
  if (!TailHeld)
    releaseSelf();
}

int append(Destructor Function, void *Object, void *Dso, bool Salted) {
  ThreadState *T = threadState(/*Create=*/true);
  if (!T || !Function || !retain(*T, reinterpret_cast<void *>(Function)) ||
      !retain(*T, Dso))
    return -1;
  auto *D = static_cast<Dtor *>(crtAlloc(sizeof(Dtor)));
  if (!D)
    return -1;
  *D = {T->Dtors, encodeExit(shared().Cookie, Function), Object, Dso, Salted};
  T->Dtors = D;
  return 0;
}

} // namespace

// Each node leaves the list before it is freed, and the modules before they
// are released, so that a rerun after a kill frees and releases nothing twice.
void wincrt::tls::dropThread(ThreadState &T) {
  for (Dtor *D; (D = T.Dtors);) {
    T.Dtors = D->Next;
    crtFree(D);
  }
  releaseLater(__atomic_exchange_n(&T.Modules, nullptr, __ATOMIC_ACQ_REL));
  T = {};
}

// Not the first callback of the directory, which is left to the program. The
// directory is linked with it, since the runtime has no thread-local data to
// name it. Under -static the runtime is the executable's, and its callback
// the executable's last.
#pragma comment(linker, "/include:_tls_used")
#ifdef COMPILER_RT_SHARED_LIB
#define WINCRT_THREAD_CALLBACK ".CRT$XLC"
#else
#define WINCRT_THREAD_CALLBACK ".CRT$XLY"
#endif
#pragma section(WINCRT_THREAD_CALLBACK, read)
extern "C" __declspec(allocate(WINCRT_THREAD_CALLBACK))
const PIMAGE_TLS_CALLBACK __wincrt_thread_callback = threadCallback;

// What the executable's last callback calls, clang_rt.wincrt.lib's tail
// object's, where the executable keeps it: found at founding.
extern "C" WINCRT_API WINCRT_NO_TP void NTAPI __wincrt_thread_end(void *,
                                                                  DWORD Reason,
                                                                  void *) {
  if (Reason == DLL_THREAD_DETACH)
    releaseSelf();
}

void wincrt::tls::findTail() {
  char *Exe = executable();
  auto *Nt = reinterpret_cast<IMAGE_NT_HEADERS *>(
      Exe + reinterpret_cast<IMAGE_DOS_HEADER *>(Exe)->e_lfanew);
  const IMAGE_DATA_DIRECTORY &Tls =
      Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  auto Tail = reinterpret_cast<PIMAGE_TLS_CALLBACK>(
      GetProcAddress(reinterpret_cast<HMODULE>(Exe), "__wincrt_thread_tail"));
  if (!Tls.VirtualAddress || !Tail)
    return;
  auto *Callback = reinterpret_cast<PIMAGE_TLS_CALLBACK *>(
      reinterpret_cast<IMAGE_TLS_DIRECTORY *>(Exe + Tls.VirtualAddress)
          ->AddressOfCallBacks);
  while (Callback && *Callback)
    TailHeld = *Callback++ == Tail;
}

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
    finalizeThread(/*UnderLoaderLock=*/false);
  else if (ThreadState *T = threadState(/*Create=*/false))
    drain(*T, Dso);
}

} // extern "C"
