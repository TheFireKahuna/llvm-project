//===-- cxa_thread_atexit.cpp - Thread-local destructor support -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements __cxa_thread_atexit_impl per Itanium C++ ABI 3.3.5.3.
//
// Uses PE TLS and TLS callbacks (matching MSVC vcruntime). FLS was considered
// but has per-fiber semantics incompatible with C++ thread_local.
//
//===----------------------------------------------------------------------===//
//
// CRITICAL LIMITATION: PER-MODULE THREAD-LOCAL STORAGE
//
// TLS slots (__declspec(thread) variables) are allocated per-module. Each DLL
// or EXE that includes this code gets its own g_FirstBlock and g_CurrentBlock.
// Consequences:
//
// 1. If libc++ is statically linked into multiple DLLs in one process, each
//    DLL has independent thread_local destructor lists.
//
// 2. When a thread exits, the TLS callback runs once per module, but only the
//    module whose callback happens to run "owns" the thread's destructor
//    state from that module. Other modules' thread_local variables will NOT
//    have their destructors called.
//
// 3. This is a PE/COFF design constraint, not a bug. There is no portable
//    solution without a shared DLL or named shared memory (security concerns).
//
// SAFE CONFIGURATIONS:
// - Single EXE with no DLLs using thread_local
// - Single DLL containing all thread_local usage
// - libc++ as shared DLL (recommended)
//
// UNSAFE CONFIGURATIONS:
// - Multiple DLLs each statically linking libc++ with thread_local usage
//
// Additional TLS callback limitations:
// - Runs inside loader lock; destructors must not call LoadLibrary/FreeLibrary
// - On process exit, only the ExitProcess thread runs destructors
// - DLL unload runs destructors only on unloading thread; other threads skip
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "internal.h"

//===----------------------------------------------------------------------===//
// Multi-module detection (debug builds only)
//===----------------------------------------------------------------------===//
//
// Tracks how many modules have registered thread_local destructors. If more
// than one, emit a warning to help diagnose the unsafe configuration.

#if WINCRT_DEBUG

namespace {

// Process-wide counter in a named section to be shared across modules.
// This is best-effort detection; not foolproof.
#pragma section(".wincrt$tls", read, write, shared)
__declspec(allocate(".wincrt$tls")) volatile long g_TlsModuleCount = 0;
__declspec(allocate(".wincrt$tls")) volatile long g_TlsWarningEmitted = 0;

void checkMultiModuleTls() {
  long count = __atomic_add_fetch(&g_TlsModuleCount, 1, __ATOMIC_ACQ_REL);
  long expected = 0;
  if (count > 1 &&
      __atomic_compare_exchange_n(&g_TlsWarningEmitted, &expected, 1, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    OutputDebugStringA(
        "WINCRT WARNING: Multiple modules (" );
    // Simple decimal conversion without sprintf
    char buf[16];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    long n = count;
    do {
      *--p = '0' + (n % 10);
      n /= 10;
    } while (n > 0);
    OutputDebugStringA(p);
    OutputDebugStringA(
        ") registered thread_local destructors.\n"
        "WINCRT WARNING: This configuration is UNSAFE. Thread-local destructors\n"
        "WINCRT WARNING: may not be called for all modules on thread exit.\n"
        "WINCRT WARNING: See compiler-rt/lib/wincrt README for details.\n");
  }
}

} // namespace

#endif // WINCRT_DEBUG

namespace {

// Matches vcruntime's FUNCS_PER_NODE.
constexpr size_t kEntriesPerBlock = 30;

struct DtorEntry {
  void (*Func)(void *);
  void *Obj;
  void *Dso;
};

struct DtorBlock {
  DtorEntry Entries[kEntriesPerBlock];
  DtorBlock *Next;
  size_t Count;
};

// First block inline in TLS; threads with few thread_local never heap-allocate.
__declspec(thread) DtorBlock g_FirstBlock;
__declspec(thread) DtorBlock *g_CurrentBlock;

// Tracks unloaded DSO handles; threads skip destructors for these to avoid
// calling into unmapped memory. Entries cannot be reclaimed without knowing
// all threads from before unload time have exited. Memory is O(unique DSOs),
// bounded by total DLLs ever loaded (typically <100).

constexpr size_t kInitialDsoCapacity = 16;

struct DsoRegistry {
  void **Handles = nullptr;
  size_t Count = 0;
  size_t Capacity = 0;
};

DsoRegistry g_UnloadedDsos;
SRWLOCK g_DsoLock = SRWLOCK_INIT;
void *g_InlineDsoStorage[kInitialDsoCapacity];

// Returns true if DSO was unloaded. Conservative: returns true if in unload
// list even if address was reused by a new module (prevents use-after-free).
bool isDsoUnloaded(void *Dso) {
  if (!Dso)
    return false;

  AcquireSRWLockShared(&g_DsoLock);

  bool found = false;
  void **handles = g_UnloadedDsos.Handles;
  size_t count = g_UnloadedDsos.Count;

  for (size_t i = 0; i < count && !found; ++i) {
    if (handles[i] == Dso)
      found = true;
  }

  ReleaseSRWLockShared(&g_DsoLock);
  return found;
}

bool isDsoUnloadedLocked(void *Dso) {
  void **handles = g_UnloadedDsos.Handles;
  size_t count = g_UnloadedDsos.Count;

  for (size_t i = 0; i < count; ++i) {
    if (handles[i] == Dso)
      return true;
  }
  return false;
}

void recordDsoUnload(void *Dso) {
  if (!Dso)
    return;

  AcquireSRWLockExclusive(&g_DsoLock);

  if (isDsoUnloadedLocked(Dso)) {
    ReleaseSRWLockExclusive(&g_DsoLock);
    return;
  }

  if (!g_UnloadedDsos.Handles) {
    g_UnloadedDsos.Handles = g_InlineDsoStorage;
    g_UnloadedDsos.Capacity = kInitialDsoCapacity;
  }

  size_t count = g_UnloadedDsos.Count;
  if (count >= g_UnloadedDsos.Capacity) {
    size_t newCapacity = g_UnloadedDsos.Capacity * 2;
    void **newHandles = static_cast<void **>(
        HeapAlloc(GetProcessHeap(), 0, newCapacity * sizeof(void *)));

    if (!newHandles) {
      // Allocation failure is non-fatal; we just won't track this DSO.
      // Worst case: destructor called into unmapped memory on thread exit.
      ReleaseSRWLockExclusive(&g_DsoLock);
      WINCRT_FATAL("DSO registry allocation failed - thread_local destructors "
                   "may call unmapped memory");
      return;
    }

    for (size_t i = 0; i < count; ++i)
      newHandles[i] = g_UnloadedDsos.Handles[i];

    if (g_UnloadedDsos.Handles != g_InlineDsoStorage)
      HeapFree(GetProcessHeap(), 0, g_UnloadedDsos.Handles);

    g_UnloadedDsos.Handles = newHandles;
    g_UnloadedDsos.Capacity = newCapacity;
  }

  g_UnloadedDsos.Handles[count] = Dso;
  g_UnloadedDsos.Count = count + 1;

  ReleaseSRWLockExclusive(&g_DsoLock);
}

DtorBlock *allocateBlock() {
  void *Mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DtorBlock));
  return static_cast<DtorBlock *>(Mem);
}

void freeBlock(DtorBlock *Block) {
  if (Block)
    HeapFree(GetProcessHeap(), 0, Block);
}

// LIFO order per Itanium ABI. Entries cleared before calling to prevent
// double-call on throw or reentry.
void runDestructors(void *Dso) {
  if (!g_CurrentBlock)
    return;

  DtorBlock *Block = g_CurrentBlock;

  while (Block) {
    for (size_t i = Block->Count; i > 0;) {
      --i;
      DtorEntry *Entry = &Block->Entries[i];

      if (!Entry->Func)
        continue;

      if (Dso && Entry->Dso != Dso)
        continue;

      if (Entry->Dso && isDsoUnloaded(Entry->Dso)) {
        Entry->Func = nullptr;
        continue;
      }

      void (*Func)(void *) = Entry->Func;
      void *Obj = Entry->Obj;
      Entry->Func = nullptr;

      wincrt::invokeDestructor(Func, Obj, "__cxa_thread_finalize");
    }

    Block = Block->Next;
  }

  if (!Dso) {
    Block = g_CurrentBlock;
    while (Block && Block != &g_FirstBlock) {
      DtorBlock *Next = Block->Next;
      freeBlock(Block);
      Block = Next;
    }

    g_FirstBlock.Count = 0;
    g_FirstBlock.Next = nullptr;
    g_CurrentBlock = nullptr;
  }
}

// TLS callback: runs INSIDE loader lock.
void __stdcall tlsCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_THREAD_DETACH || Reason == DLL_PROCESS_DETACH) {
    runDestructors(nullptr);
  }
}

} // anonymous namespace

#pragma section(".CRT$XLC", long, read)

extern "C" {

// .CRT$XLC: after dynamic TLS init (.CRT$XLB), before other callbacks.
__declspec(allocate(".CRT$XLC")) PIMAGE_TLS_CALLBACK
    __cxa_thread_atexit_callback = tlsCallback;

#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:___cxa_thread_atexit_callback")
#else
#pragma comment(linker, "/INCLUDE:__cxa_thread_atexit_callback")
#endif

}

extern "C" {

int __cxa_thread_atexit_impl(void (*Func)(void *), void *Obj, void *Dso) {
  if (!Func)
    return -1;

  if (!g_CurrentBlock) {
    g_FirstBlock.Count = 0;
    g_FirstBlock.Next = nullptr;
    g_CurrentBlock = &g_FirstBlock;

#if WINCRT_DEBUG
    // First registration in this module on this thread. Check for multi-module
    // usage which is unsafe.
    static volatile long s_CheckDone = 0;
    long expected = 0;
    if (__atomic_compare_exchange_n(&s_CheckDone, &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      checkMultiModuleTls();
    }
#endif
  }

  if (g_CurrentBlock->Count >= kEntriesPerBlock) {
    DtorBlock *NewBlock = allocateBlock();
    if (!NewBlock)
      return -1;
    NewBlock->Next = g_CurrentBlock;
    g_CurrentBlock = NewBlock;
  }

  DtorEntry *Entry = &g_CurrentBlock->Entries[g_CurrentBlock->Count++];
  Entry->Func = Func;
  Entry->Obj = Obj;
  Entry->Dso = Dso;

  return 0;
}

void __cxa_thread_finalize(void *Dso) {
  runDestructors(Dso);
}

// Run destructors and record DSO as unloaded. Other threads skip destructors
// for this DSO when they exit (may leak resources, but prevents use-after-free).
void __cxa_thread_finalize_dso_unload(void *Dso) {
  if (!Dso)
    return;

  runDestructors(Dso);
  recordDsoUnload(Dso);
}

}

#endif // _WIN32
