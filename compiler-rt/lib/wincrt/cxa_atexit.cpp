//===-- cxa_atexit.cpp - Itanium ABI atexit handlers ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Fallback __cxa_atexit/__cxa_finalize when LLVM libc is not linked.
// LLVM libc's __cxa_finalize ignores the DSO parameter; this fallback handles
// it correctly for per-DLL unload cleanup.
// Ref: https://itanium-cxx-abi.github.io/cxx-abi/abi.html#dso-dtor
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "internal.h"

namespace {

using wincrt::DtorBlock;
using wincrt::DtorEntry;

DtorBlock g_firstBlock = {};
DtorBlock* g_currentBlock = &g_firstBlock;
SRWLOCK g_lock = SRWLOCK_INIT;

class SRWLockGuard {
  PSRWLOCK lock_;
public:
  explicit SRWLockGuard(PSRWLOCK lock) : lock_(lock) {
    AcquireSRWLockExclusive(lock_);
  }
  ~SRWLockGuard() {
    if (lock_)
      ReleaseSRWLockExclusive(lock_);
  }
  void release() {
    ReleaseSRWLockExclusive(lock_);
    lock_ = nullptr;
  }
  void reacquire(PSRWLOCK lock) {
    lock_ = lock;
    AcquireSRWLockExclusive(lock_);
  }
  SRWLockGuard(const SRWLockGuard&) = delete;
  SRWLockGuard& operator=(const SRWLockGuard&) = delete;
};

int wincrtCxaAtexitImpl(void (*dtor)(void*), void* obj, void* dso) {
  if (!dtor)
    return -1;

  SRWLockGuard guard(&g_lock);

  if (!g_currentBlock->hasSpace()) {
    DtorBlock* newBlock = wincrt::allocateDtorBlock();
    if (!newBlock)
      return -1;
    newBlock->Next = g_currentBlock;
    g_currentBlock = newBlock;
  }

  g_currentBlock->push(dtor, obj, dso);
  return 0;
}

// Lock released during callbacks to prevent deadlock if callback registers
// new handlers. Per Itanium ABI, multiple calls to __cxa_finalize shall not
// result in calling entries multiple times.
void wincrtCxaFinalizeImpl(void* dso) {
  SRWLockGuard guard(&g_lock);

  DtorBlock* block = g_currentBlock;
  size_t idx = block ? block->Count : 0;

  while (block) {
    while (idx > 0) {
      --idx;
      DtorEntry& entry = block->Entries[idx];

      if (dso != nullptr && entry.Dso != dso)
        continue;

      if (!entry.Dtor)
        continue;

      void (*dtor)(void*) = entry.Dtor;
      void* obj = entry.Obj;
      entry.Dtor = nullptr;

      DtorBlock* savedBlock = block;
      size_t savedCount = g_currentBlock->Count;

      guard.release();
      wincrt::invokeDestructor(dtor, obj, "__cxa_finalize");
      guard.reacquire(&g_lock);

      // Restart from new entries added during callback (maintains LIFO order).
      if (g_currentBlock != savedBlock) {
        block = g_currentBlock;
        idx = block->Count;
        continue;
      }
      if (g_currentBlock->Count > savedCount) {
        idx = g_currentBlock->Count;
        continue;
      }
    }

    block = block->Next;
    if (block)
      idx = block->Count;
  }

  // Cleanup empty blocks (keep g_firstBlock, it's static).
  if (dso == nullptr) {
    DtorBlock* prev = nullptr;
    block = g_currentBlock;
    while (block != &g_firstBlock) {
      DtorBlock* next = block->Next;
      bool allCalled = true;
      for (size_t i = 0; i < block->Count; ++i) {
        if (block->Entries[i].Dtor) {
          allCalled = false;
          break;
        }
      }
      if (allCalled) {
        if (prev)
          prev->Next = next;
        else
          g_currentBlock = next;
        wincrt::freeDtorBlock(block);
      } else {
        prev = block;
      }
      block = next;
    }
    bool allCalled = true;
    for (size_t i = 0; i < g_firstBlock.Count; ++i) {
      if (g_firstBlock.Entries[i].Dtor) {
        allCalled = false;
        break;
      }
    }
    if (allCalled) {
      g_firstBlock.Count = 0;
      g_currentBlock = &g_firstBlock;
    }
  }
}

} // anonymous namespace

extern "C" {

// Define the Itanium ABI functions directly.
// TODO: Add weak linkage via alternatename when lld-link crash is fixed.
int __cdecl __cxa_atexit(void (*dtor)(void*), void* obj, void* dso) {
  return wincrtCxaAtexitImpl(dtor, obj, dso);
}

void __cdecl __cxa_finalize(void* dso) {
  wincrtCxaFinalizeImpl(dso);
}

}

#endif // _WIN32
