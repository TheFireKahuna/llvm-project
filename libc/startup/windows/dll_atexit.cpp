//===-- Standalone __cxa_atexit/__cxa_finalize for DLL CRT --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Self-contained atexit for DLLs built before libc (libunwind, libc++).
// Uses SRW lock (zero-init, no deps) and CommitVector (NT VA, no malloc).
// Provides both Itanium ABI (__cxa_atexit/__cxa_finalize) and C atexit so
// the bootstrap C++ runtimes can stay self-contained before full libc links.
//
//===----------------------------------------------------------------------===//

// NT virtual memory for growable array. NtAllocateVirtualMemoryEx is
// from ntdll.dll (always loaded).
extern "C" {
typedef long NTSTATUS;                         /* NT status code */
typedef unsigned long ULONG;
typedef void *PVOID;
typedef void *HANDLE;
#define NT_SUCCESS(s) ((s) >= 0)
#define NtCurrentProcess() ((HANDLE)(long long)-1)
#define MEM_RESERVE 0x00002000
#define MEM_COMMIT  0x00001000
#define PAGE_READWRITE 0x04
#define PAGE_NOACCESS  0x01

__declspec(dllimport) NTSTATUS __stdcall
NtAllocateVirtualMemoryEx(HANDLE, PVOID *, unsigned long long *,
                          ULONG, ULONG, void *, ULONG);

// SRW lock — ntdll.dll, zero-init, no setup needed.
// Use the native Rtl* names directly (kernel32 versions are just forwarders).
struct SRWLOCK { void *Ptr; };
__declspec(dllimport) void __stdcall RtlAcquireSRWLockExclusive(SRWLOCK *);
__declspec(dllimport) void __stdcall RtlReleaseSRWLockExclusive(SRWLOCK *);
} // extern "C"

namespace {

using AtExitFn = void (*)(void *);
using StdAtExitFn = void (*)(void);

struct AtExitEntry {
  AtExitFn callback;
  void *payload;
  void *dso;
};

// Reserve 256KB upfront — room for ~10,000 entries. Physical pages are
// committed one 4KB page at a time as entries are added.
constexpr unsigned long long RESERVE_BYTES = 256 * 1024;
constexpr unsigned PAGE_SIZE = 4096;

SRWLOCK g_lock = {nullptr};

AtExitEntry *g_entries = nullptr;
unsigned g_size = 0;
unsigned g_committed = 0;  // entries backed by committed pages
unsigned g_reserved = 0;   // total entries in reserved VA range

bool ensure_capacity() {
  if (g_size < g_committed)
    return true;
  if (!g_entries) {
    // Reserve large VA range — no physical backing yet.
    void *addr = nullptr;
    unsigned long long sz = RESERVE_BYTES;
    NTSTATUS st = NtAllocateVirtualMemoryEx(
        NtCurrentProcess(), &addr, &sz, MEM_RESERVE,
        PAGE_NOACCESS, nullptr, 0);
    if (!NT_SUCCESS(st))
      return false;
    g_entries = static_cast<AtExitEntry *>(addr);
    g_reserved = static_cast<unsigned>(sz / sizeof(AtExitEntry));
  }
  // Commit one more page within the reservation.
  if (g_committed >= g_reserved)
    return false;
  void *next_page = g_entries + g_committed;
  unsigned long long sz = PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &next_page, &sz, MEM_COMMIT,
      PAGE_READWRITE, nullptr, 0);
  if (!NT_SUCCESS(st))
    return false;
  g_committed += static_cast<unsigned>(sz / sizeof(AtExitEntry));
  return true;
}

void stdc_atexit_thunk(void *payload) {
  reinterpret_cast<StdAtExitFn>(payload)();
}

} // namespace

extern "C" {

extern void *__dso_handle;

int __cxa_atexit(AtExitFn callback, void *payload, void *dso) {
  RtlAcquireSRWLockExclusive(&g_lock);
  bool ok = ensure_capacity();
  if (ok)
    g_entries[g_size++] = {callback, payload, dso};
  RtlReleaseSRWLockExclusive(&g_lock);
  return ok ? 0 : -1;
}

int atexit(StdAtExitFn callback) {
  return __cxa_atexit(&stdc_atexit_thunk, reinterpret_cast<void *>(callback),
                      __dso_handle);
}

void __cxa_finalize(void *dso) {
  RtlAcquireSRWLockExclusive(&g_lock);
  if (!dso) {
    // Process/DLL exit: call all in reverse order.
    while (g_size > 0) {
      AtExitEntry e = g_entries[--g_size];
      RtlReleaseSRWLockExclusive(&g_lock);
      if (e.callback)
        e.callback(e.payload);
      RtlAcquireSRWLockExclusive(&g_lock);
    }
  } else {
    // Per-DSO finalize: call and tombstone matching entries.
    for (unsigned i = g_size; i > 0; --i) {
      AtExitEntry &e = g_entries[i - 1];
      if (e.callback && e.dso == dso) {
        AtExitFn cb = e.callback;
        void *pl = e.payload;
        e.callback = nullptr; // tombstone
        RtlReleaseSRWLockExclusive(&g_lock);
        cb(pl);
        RtlAcquireSRWLockExclusive(&g_lock);
      }
    }
  }
  RtlReleaseSRWLockExclusive(&g_lock);
}

} // extern "C"
