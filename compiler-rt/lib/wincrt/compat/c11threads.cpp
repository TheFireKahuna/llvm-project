//===-- c11threads.cpp - C11 threads implementation -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C11 <threads.h> for Windows Itanium using modern Win32 primitives.
///
/// Design:
///   - WaitOnAddress (Win 8+) for timed mutex/sleep without spin-polling
///   - SRWLOCK for non-recursive mutexes (zero-init, no allocation)
///   - CRITICAL_SECTION for recursive mutexes
///   - CONDITION_VARIABLE for condition variables
///   - TLS callback (.CRT$XLD) for TSS destructor dispatch with iteration
///   - InitOnceExecuteOnce for call_once
///   - FLS for thrd_current handle storage
///
/// Type layouts match MSVC <threads.h> for ABI compatibility.
///
/// Requires: Windows 8.1+ (WaitOnAddress, CONDITION_VARIABLE improvements)
///
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "../internal.h"
#include <process.h>

// Constants

namespace {

constexpr DWORD kInfinite = 0xFFFFFFFF;
constexpr DWORD kWaitObject0 = 0;
constexpr DWORD kWaitTimeout = 258;          // WAIT_TIMEOUT
constexpr DWORD kErrorTimeout = 0x5B4;       // ERROR_TIMEOUT
constexpr DWORD kTlsOutOfIndexes = 0xFFFFFFFF;
constexpr DWORD kFlsOutOfIndexes = 0xFFFFFFFF;
constexpr ULONG kCondVarLockExclusive = 0;

// FILETIME: 100ns ticks since 1601-01-01
// Unix epoch: 1970-01-01
constexpr uint64_t kFileTimeEpochDelta = 116444736000000000ULL;
constexpr uint64_t kFileTimeTicksPerSec = 10000000ULL;
constexpr uint64_t kFileTimeTicksPerMs = 10000ULL;
constexpr uint64_t kNsPerFileTimeTick = 100ULL;
constexpr int64_t kNsPerSec = 1000000000LL;
constexpr int64_t kNsPerMs = 1000000LL;
constexpr int64_t kMaxTimespecSec = INT64_MAX / kNsPerSec;

// C11 7.26.1: TSS_DTOR_ITERATIONS shall be at least 4.
constexpr int kTssDtorIterations = 4;

// POSIX requires PTHREAD_KEYS_MAX >= 128. Match for compatibility.
constexpr size_t kMaxTssKeys = 128;

} // namespace

// Timespec Types (32/64-bit time_t ABI)

struct Timespec32 {
  int32_t tv_sec;
  long tv_nsec;
};

struct Timespec64 {
  int64_t tv_sec;
  long tv_nsec;
};

// C11 Type Definitions
//
// Layouts must match MSVC <threads.h> for ABI compatibility with code compiled
// by MSVC that passes these types across translation unit boundaries.

/// Mutex type flags (C11 7.26.4).
enum {
  mtx_plain = 0,
  mtx_recursive = 1,
  mtx_timed = 2,
};

/// Internal mutex kind for dispatch.
enum MtxKind : uint32_t {
  kMtxSrwPlain = 0,
  kMtxSrwTimed = 1,
  kMtxCsRecursive = 2,
  kMtxCsTimedRecursive = 3,
};

/// Mutex structure with inline storage for CRITICAL_SECTION.
struct alignas(void *) wincrt_mtx_t {
  uint32_t Kind;
  _Atomic(uint32_t) Waiters; // Waiter count for timed mutexes
  union {
    SRWLOCK Srw;
    CRITICAL_SECTION Cs;
  };
};

struct wincrt_cnd_t {
  CONDITION_VARIABLE Cv;
};

struct wincrt_thrd_t {
  void *Handle;
  uint32_t Tid;
};

struct wincrt_tss_t {
  uint32_t Key;
};

struct wincrt_once_flag {
  INIT_ONCE Once;
};

/// C11 7.26.1: Thread function return codes.
enum {
  thrd_success = 0,
  thrd_nomem = 1,
  thrd_timedout = 2,
  thrd_busy = 3,
  thrd_error = 4,
};

// Time Helpers

namespace {

/// Validate timespec per C11 7.27.2.5: tv_nsec in [0, 999999999].
inline bool isValidTimespec(const Timespec64 *ts) {
  return ts && ts->tv_nsec >= 0 && ts->tv_nsec < kNsPerSec;
}

/// Query performance counter frequency (cached).
int64_t getQpcFrequency() {
  static int64_t Freq = [] {
    LARGE_INTEGER F;
    QueryPerformanceFrequency(&F);
    return F.QuadPart;
  }();
  return Freq;
}

/// Monotonic nanoseconds via QueryPerformanceCounter.
int64_t getMonotonicNs() {
  LARGE_INTEGER Counter;
  QueryPerformanceCounter(&Counter);
  int64_t CounterVal = Counter.QuadPart;
  int64_t Freq = getQpcFrequency();
  // Avoid overflow: split computation for large counter values.
  if (CounterVal > kMaxTimespecSec)
    return (CounterVal / Freq) * kNsPerSec + (CounterVal % Freq) * kNsPerSec / Freq;
  return CounterVal * kNsPerSec / Freq;
}

/// Convert absolute TIME_UTC timespec to relative milliseconds.
/// Returns 0 if deadline passed, kInfinite-1 if overflow.
DWORD timespecToRelativeMs(const Timespec64 *ts) {
  if (!ts)
    return kInfinite;

  // Clamp negative to zero (deadline passed).
  if (ts->tv_sec < 0)
    return 0;

  // Convert to FILETIME ticks, clamping overflow.
  uint64_t TargetTicks;
  if (static_cast<uint64_t>(ts->tv_sec) > (UINT64_MAX - kFileTimeEpochDelta) / kFileTimeTicksPerSec) {
    TargetTicks = UINT64_MAX;
  } else {
    TargetTicks = static_cast<uint64_t>(ts->tv_sec) * kFileTimeTicksPerSec +
                  static_cast<uint64_t>(ts->tv_nsec) / kNsPerFileTimeTick +
                  kFileTimeEpochDelta;
  }

  FILETIME NowFt;
  GetSystemTimeAsFileTime(&NowFt);
  uint64_t NowTicks = (static_cast<uint64_t>(NowFt.dwHighDateTime) << 32) |
                      NowFt.dwLowDateTime;

  if (TargetTicks <= NowTicks)
    return 0;

  uint64_t DiffMs = (TargetTicks - NowTicks) / kFileTimeTicksPerMs;
  return DiffMs >= kInfinite ? kInfinite - 1 : static_cast<DWORD>(DiffMs);
}

/// Remaining timeout from monotonic deadline.
DWORD remainingMs(int64_t DeadlineNs) {
  int64_t NowNs = getMonotonicNs();
  if (NowNs >= DeadlineNs)
    return 0;
  int64_t DiffMs = (DeadlineNs - NowNs) / kNsPerMs;
  return DiffMs >= static_cast<int64_t>(kInfinite) ? kInfinite - 1
                                                    : static_cast<DWORD>(DiffMs);
}

inline bool isRecursive(int Type) { return (Type & mtx_recursive) != 0; }
inline bool isTimed(int Type) { return (Type & mtx_timed) != 0; }

} // namespace

// Per-Thread State
//
// Stores thread handle for thrd_current() and optional interrupt flag.
// Allocated in FLS for automatic cleanup on thread exit.

namespace {

struct ThreadState {
  void *Handle;            // Duplicated handle for thrd_current()
  uint32_t Tid;
  _Atomic(uint32_t) InterruptFlag;
};

void __stdcall threadStateFlsCleanup(void *Ptr) {
  if (!Ptr)
    return;
  auto *State = static_cast<ThreadState *>(Ptr);
  if (State->Handle)
    CloseHandle(State->Handle);
  wincrt::crtFree(State);
}

DWORD getThreadStateFls() {
  static DWORD Slot = FlsAlloc(threadStateFlsCleanup);
  return Slot;
}

ThreadState *getThreadState() {
  DWORD Slot = getThreadStateFls();
  if (Slot == kFlsOutOfIndexes)
    return nullptr;
  return static_cast<ThreadState *>(FlsGetValue(Slot));
}

ThreadState *getOrCreateThreadState() {
  DWORD Slot = getThreadStateFls();
  if (Slot == kFlsOutOfIndexes)
    return nullptr;

  auto *State = static_cast<ThreadState *>(FlsGetValue(Slot));
  if (State)
    return State;

  State = static_cast<ThreadState *>(wincrt::crtAlloc(sizeof(ThreadState)));
  if (!State)
    return nullptr;

  State->Handle = nullptr;
  State->Tid = GetCurrentThreadId();
  __c11_atomic_init(&State->InterruptFlag, 0);
  FlsSetValue(Slot, State);
  return State;
}

} // namespace

// Thread Interrupt Map
//
// Maps thread ID to ThreadState* for cross-thread interrupt delivery.
// Dynamic resizing hash table with linear probing.

namespace {

struct InterruptMap {
  struct Entry {
    _Atomic(DWORD) Tid;      // 0 = empty, ~0 = tombstone
    _Atomic(ThreadState *) State;
  };

  static constexpr DWORD kEmpty = 0;
  static constexpr DWORD kTombstone = ~0U;
  static constexpr size_t kInitialCapacity = 64;
  static constexpr size_t kMaxCapacity = 1 << 20; // 1M threads

  Entry *Table;
  size_t Capacity;
  _Atomic(size_t) Count;
  SRWLOCK Lock;

  void init() {
    Table = static_cast<Entry *>(
        wincrt::crtAlloc(kInitialCapacity * sizeof(Entry)));
    Capacity = kInitialCapacity;
    __c11_atomic_init(&Count, 0);
    for (size_t I = 0; I < Capacity; ++I) {
      __c11_atomic_init(&Table[I].Tid, kEmpty);
      __c11_atomic_init(&Table[I].State, nullptr);
    }
  }

  void grow() {
    // Called with lock held.
    size_t NewCap = Capacity * 2;
    if (NewCap > kMaxCapacity)
      return; // At max, insertions will fail.

    auto *NewTable = static_cast<Entry *>(wincrt::crtAlloc(NewCap * sizeof(Entry)));
    if (!NewTable)
      return;

    for (size_t I = 0; I < NewCap; ++I) {
      __c11_atomic_init(&NewTable[I].Tid, kEmpty);
      __c11_atomic_init(&NewTable[I].State, nullptr);
    }

    // Rehash existing entries.
    for (size_t I = 0; I < Capacity; ++I) {
      DWORD Tid = __c11_atomic_load(&Table[I].Tid, __ATOMIC_RELAXED);
      if (Tid == kEmpty || Tid == kTombstone)
        continue;
      ThreadState *St = __c11_atomic_load(&Table[I].State, __ATOMIC_RELAXED);

      size_t Idx = Tid % NewCap;
      for (size_t J = 0; J < NewCap; ++J) {
        size_t Slot = (Idx + J) % NewCap;
        DWORD SlotTid = __c11_atomic_load(&NewTable[Slot].Tid, __ATOMIC_RELAXED);
        if (SlotTid == kEmpty) {
          __c11_atomic_store(&NewTable[Slot].Tid, Tid, __ATOMIC_RELAXED);
          __c11_atomic_store(&NewTable[Slot].State, St, __ATOMIC_RELAXED);
          break;
        }
      }
    }

    wincrt::crtFree(Table);
    Table = NewTable;
    Capacity = NewCap;
  }

  bool insert(DWORD Tid, ThreadState *State) {
    AcquireSRWLockExclusive(&Lock);

    // Grow if load factor > 0.5.
    size_t Cnt = __c11_atomic_load(&Count, __ATOMIC_RELAXED);
    if (Cnt * 2 >= Capacity)
      grow();

    size_t Idx = Tid % Capacity;
    size_t TombstoneSlot = Capacity;
    bool Inserted = false;

    for (size_t I = 0; I < Capacity; ++I) {
      size_t Slot = (Idx + I) % Capacity;
      DWORD SlotTid = __c11_atomic_load(&Table[Slot].Tid, __ATOMIC_RELAXED);

      if (SlotTid == Tid) {
        // Update existing.
        __c11_atomic_store(&Table[Slot].State, State, __ATOMIC_RELEASE);
        Inserted = true;
        break;
      }
      if (SlotTid == kTombstone && TombstoneSlot == Capacity) {
        TombstoneSlot = Slot;
      }
      if (SlotTid == kEmpty) {
        size_t UseSlot = (TombstoneSlot != Capacity) ? TombstoneSlot : Slot;
        __c11_atomic_store(&Table[UseSlot].Tid, Tid, __ATOMIC_RELAXED);
        __c11_atomic_store(&Table[UseSlot].State, State, __ATOMIC_RELEASE);
        __c11_atomic_fetch_add(&Count, 1, __ATOMIC_RELAXED);
        Inserted = true;
        break;
      }
    }

    ReleaseSRWLockExclusive(&Lock);
    return Inserted;
  }

  void remove(DWORD Tid) {
    AcquireSRWLockExclusive(&Lock);

    size_t Idx = Tid % Capacity;
    for (size_t I = 0; I < Capacity; ++I) {
      size_t Slot = (Idx + I) % Capacity;
      DWORD SlotTid = __c11_atomic_load(&Table[Slot].Tid, __ATOMIC_RELAXED);

      if (SlotTid == Tid) {
        __c11_atomic_store(&Table[Slot].Tid, kTombstone, __ATOMIC_RELAXED);
        __c11_atomic_store(&Table[Slot].State, nullptr, __ATOMIC_RELAXED);
        __c11_atomic_fetch_sub(&Count, 1, __ATOMIC_RELAXED);
        break;
      }
      if (SlotTid == kEmpty)
        break;
    }

    ReleaseSRWLockExclusive(&Lock);
  }

  ThreadState *lookup(DWORD Tid) {
    AcquireSRWLockShared(&Lock);

    ThreadState *Result = nullptr;
    size_t Idx = Tid % Capacity;
    for (size_t I = 0; I < Capacity; ++I) {
      size_t Slot = (Idx + I) % Capacity;
      DWORD SlotTid = __c11_atomic_load(&Table[Slot].Tid, __ATOMIC_ACQUIRE);

      if (SlotTid == Tid) {
        Result = __c11_atomic_load(&Table[Slot].State, __ATOMIC_ACQUIRE);
        break;
      }
      if (SlotTid == kEmpty)
        break;
    }

    ReleaseSRWLockShared(&Lock);
    return Result;
  }
};

InterruptMap g_InterruptMap;

} // namespace

// Thread Functions (thrd_*)

namespace {

struct ThreadStartContext {
  int (*Func)(void *);
  void *Arg;
  void *CallerHandle; // Handle duplicated by caller for thrd_current()
};

unsigned __stdcall threadWrapper(void *Param) {
  auto *Ctx = static_cast<ThreadStartContext *>(Param);
  int (*Func)(void *) = Ctx->Func;
  void *Arg = Ctx->Arg;
  void *Handle = Ctx->CallerHandle;
  wincrt::crtFree(Ctx);

  // Set up thread state for thrd_current().
  ThreadState *State = getOrCreateThreadState();
  if (State) {
    State->Handle = Handle;
    State->Tid = GetCurrentThreadId();
    g_InterruptMap.insert(State->Tid, State);
  }

  int Result = Func(Arg);

  // Cleanup before exit.
  if (State)
    g_InterruptMap.remove(State->Tid);

  return static_cast<unsigned>(Result);
}

} // namespace

extern "C" {

/// C11 7.26.5.1: Creates a new thread.
int __cdecl thrd_create(wincrt_thrd_t *Thr, int (*Func)(void *), void *Arg) {
  WINCRT_ASSERT(Thr && Func);

  auto *Ctx = static_cast<ThreadStartContext *>(
      wincrt::crtAlloc(sizeof(ThreadStartContext)));
  if (!Ctx)
    return thrd_nomem;

  Ctx->Func = Func;
  Ctx->Arg = Arg;
  Ctx->CallerHandle = nullptr;

  unsigned Tid = 0;
  uintptr_t Handle = _beginthreadex(nullptr, 0, threadWrapper, Ctx, 0, &Tid);
  if (Handle == 0) {
    wincrt::crtFree(Ctx);
    return thrd_error;
  }

  // Duplicate handle for the new thread's thrd_current().
  HANDLE Dup = nullptr;
  HANDLE Proc = GetCurrentProcess();
  if (DuplicateHandle(Proc, reinterpret_cast<HANDLE>(Handle), Proc, &Dup,
                      0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */)) {
    Ctx->CallerHandle = Dup;
  }

  Thr->Handle = reinterpret_cast<void *>(Handle);
  Thr->Tid = Tid;
  return thrd_success;
}

/// C11 7.26.5.2: Returns identifier of calling thread.
///
/// The returned thrd_t is valid for all operations including thrd_join and
/// thrd_detach (unlike POSIX pthread_self which requires pthread_create).
wincrt_thrd_t __cdecl thrd_current(void) {
  wincrt_thrd_t T;
  ThreadState *State = getThreadState();
  if (State && State->Handle) {
    T.Handle = State->Handle;
    T.Tid = State->Tid;
  } else {
    // Main thread or thread not created by thrd_create.
    T.Handle = nullptr;
    T.Tid = GetCurrentThreadId();
  }
  return T;
}

/// C11 7.26.5.3: Detaches thread.
int __cdecl thrd_detach(wincrt_thrd_t Thr) {
  if (!Thr.Handle)
    return thrd_error;
  return CloseHandle(Thr.Handle) ? thrd_success : thrd_error;
}

/// C11 7.26.5.4: Tests thread equality.
int __cdecl thrd_equal(wincrt_thrd_t A, wincrt_thrd_t B) {
  return A.Tid == B.Tid;
}

/// C11 7.26.5.5: Terminates calling thread.
WINCRT_NORETURN void __cdecl thrd_exit(int Res) {
  ThreadState *State = getThreadState();
  if (State)
    g_InterruptMap.remove(State->Tid);
  _endthreadex(static_cast<unsigned>(Res));
  __builtin_unreachable();
}

/// C11 7.26.5.6: Joins with thread.
int __cdecl thrd_join(wincrt_thrd_t Thr, int *Res) {
  if (!Thr.Handle)
    return thrd_error;

  if (WaitForSingleObject(Thr.Handle, kInfinite) != kWaitObject0) {
    CloseHandle(Thr.Handle);
    return thrd_error;
  }

  if (Res) {
    DWORD Code = 0;
    GetExitCodeThread(Thr.Handle, &Code);
    *Res = static_cast<int>(Code);
  }

  CloseHandle(Thr.Handle);
  return thrd_success;
}

/// C11 7.26.5.7: Suspends execution for duration.
///
/// Returns:
///   0 on timeout elapsed
///  -1 on interrupt (remaining time in *Rem)
///  -2 on error
int __cdecl _thrd_sleep64(const Timespec64 *Dur, Timespec64 *Rem) {
  WINCRT_ASSERT(Dur);

  if (Dur->tv_sec < 0 || Dur->tv_nsec < 0 || Dur->tv_nsec >= kNsPerSec)
    return -2;

  if (Dur->tv_sec == 0 && Dur->tv_nsec == 0) {
    if (Rem) {
      Rem->tv_sec = 0;
      Rem->tv_nsec = 0;
    }
    return 0;
  }

  int64_t TotalNs = static_cast<int64_t>(Dur->tv_sec) * kNsPerSec +
                    static_cast<int64_t>(Dur->tv_nsec);
  int64_t StartNs = getMonotonicNs();
  int64_t DeadlineNs = StartNs + TotalNs;

  ThreadState *State = getOrCreateThreadState();
  if (!State) {
    // Fallback: uninterruptible sleep.
    DWORD Ms = TotalNs / kNsPerMs;
    Sleep(Ms > 0 ? Ms : 1);
    if (Rem) {
      Rem->tv_sec = 0;
      Rem->tv_nsec = 0;
    }
    return 0;
  }

  // Check pending interrupt.
  if (__c11_atomic_exchange(&State->InterruptFlag, 0, __ATOMIC_ACQ_REL)) {
    if (Rem) {
      Rem->tv_sec = Dur->tv_sec;
      Rem->tv_nsec = Dur->tv_nsec;
    }
    return -1;
  }

  // Wait loop with WaitOnAddress.
  uint32_t Expected = 0;
  for (;;) {
    DWORD Timeout = remainingMs(DeadlineNs);
    if (Timeout == 0)
      break;

    WaitOnAddress(&State->InterruptFlag, &Expected, sizeof(uint32_t), Timeout);

    if (__c11_atomic_load(&State->InterruptFlag, __ATOMIC_ACQUIRE)) {
      __c11_atomic_store(&State->InterruptFlag, 0, __ATOMIC_RELEASE);
      if (Rem) {
        int64_t Elapsed = getMonotonicNs() - StartNs;
        int64_t Left = TotalNs > Elapsed ? TotalNs - Elapsed : 0;
        Rem->tv_sec = Left / kNsPerSec;
        Rem->tv_nsec = static_cast<long>(Left % kNsPerSec);
      }
      return -1;
    }
  }

  if (Rem) {
    Rem->tv_sec = 0;
    Rem->tv_nsec = 0;
  }
  return 0;
}

int __cdecl _thrd_sleep32(const Timespec32 *Dur, Timespec32 *Rem) {
  WINCRT_ASSERT(Dur);
  Timespec64 D64 = {Dur->tv_sec, Dur->tv_nsec};
  Timespec64 R64 = {0, 0};
  int Ret = _thrd_sleep64(&D64, Rem ? &R64 : nullptr);
  if (Rem) {
    Rem->tv_sec = static_cast<int32_t>(R64.tv_sec);
    Rem->tv_nsec = R64.tv_nsec;
  }
  return Ret;
}

int __cdecl thrd_sleep(const Timespec64 *Dur, Timespec64 *Rem) {
  return _thrd_sleep64(Dur, Rem);
}

/// C11 7.26.5.8: Yields to other threads.
void __cdecl thrd_yield(void) {
  SwitchToThread();
}

/// Extension: Interrupts a sleeping thread.
///
/// Wakes Thr if blocked in thrd_sleep, or causes next thrd_sleep to return -1.
/// Thr must have been created by thrd_create.
int __cdecl _thrd_interrupt(wincrt_thrd_t Thr) {
  ThreadState *State = g_InterruptMap.lookup(Thr.Tid);
  if (!State)
    return thrd_error;

  __c11_atomic_store(&State->InterruptFlag, 1, __ATOMIC_RELEASE);

#if defined(__aarch64__) || defined(__arm64ec__) || defined(__arm__)
  __dmb(_ARM64_BARRIER_ISH);
#endif

  WakeByAddressSingle(const_cast<_Atomic(uint32_t) *>(&State->InterruptFlag));
  return thrd_success;
}

} // extern "C"

// Mutex Functions (mtx_*)
//
// Non-recursive: SRWLOCK (slim, zero-init, no destroy needed).
// Recursive: CRITICAL_SECTION (heavier, supports recursion).
// Timed: Uses Waiters count + WaitOnAddress for efficient blocking.

extern "C" {

/// C11 7.26.4.2: Initializes mutex.
int __cdecl mtx_init(wincrt_mtx_t *Mtx, int Type) {
  WINCRT_ASSERT(Mtx);

  __c11_atomic_init(&Mtx->Waiters, 0);

  if (isRecursive(Type)) {
    InitializeCriticalSection(&Mtx->Cs);
    Mtx->Kind = isTimed(Type) ? kMtxCsTimedRecursive : kMtxCsRecursive;
  } else {
    InitializeSRWLock(&Mtx->Srw);
    Mtx->Kind = isTimed(Type) ? kMtxSrwTimed : kMtxSrwPlain;
  }

  return thrd_success;
}

/// C11 7.26.4.1: Destroys mutex.
void __cdecl mtx_destroy(wincrt_mtx_t *Mtx) {
  WINCRT_ASSERT(Mtx);
  if (Mtx->Kind == kMtxCsRecursive || Mtx->Kind == kMtxCsTimedRecursive)
    DeleteCriticalSection(&Mtx->Cs);
  // SRWLOCK has no destroy.
}

/// C11 7.26.4.3: Locks mutex.
int __cdecl mtx_lock(wincrt_mtx_t *Mtx) {
  WINCRT_ASSERT(Mtx);

  switch (Mtx->Kind) {
  case kMtxCsRecursive:
  case kMtxCsTimedRecursive:
    EnterCriticalSection(&Mtx->Cs);
    break;
  default:
    AcquireSRWLockExclusive(&Mtx->Srw);
    break;
  }
  return thrd_success;
}

/// C11 7.26.4.5: Tries to lock mutex without blocking.
int __cdecl mtx_trylock(wincrt_mtx_t *Mtx) {
  WINCRT_ASSERT(Mtx);

  BOOL Ok;
  switch (Mtx->Kind) {
  case kMtxCsRecursive:
  case kMtxCsTimedRecursive:
    Ok = TryEnterCriticalSection(&Mtx->Cs);
    break;
  default:
    Ok = TryAcquireSRWLockExclusive(&Mtx->Srw);
    break;
  }
  return Ok ? thrd_success : thrd_busy;
}

/// C11 7.26.4.6: Unlocks mutex.
int __cdecl mtx_unlock(wincrt_mtx_t *Mtx) {
  WINCRT_ASSERT(Mtx);

  // Read waiters before releasing lock to ensure visibility.
  uint32_t Waiters = __c11_atomic_load(&Mtx->Waiters, __ATOMIC_ACQUIRE);

  switch (Mtx->Kind) {
  case kMtxCsRecursive:
  case kMtxCsTimedRecursive:
    LeaveCriticalSection(&Mtx->Cs);
    break;
  default:
    ReleaseSRWLockExclusive(&Mtx->Srw);
    break;
  }

  // Wake one waiter if any.
  if (Waiters > 0)
    WakeByAddressSingle(const_cast<_Atomic(uint32_t) *>(&Mtx->Waiters));

  return thrd_success;
}

/// C11 7.26.4.4: Locks mutex with timeout.
///
/// Behavior undefined if Mtx was not created with mtx_timed.
int __cdecl _mtx_timedlock64(wincrt_mtx_t *Mtx, const Timespec64 *Ts) {
  WINCRT_ASSERT(Mtx);
  WINCRT_ASSERT(Mtx->Kind == kMtxSrwTimed || Mtx->Kind == kMtxCsTimedRecursive);

  if (!isValidTimespec(Ts))
    return thrd_error;

  // Fast path: try immediate acquisition.
  if (mtx_trylock(Mtx) == thrd_success)
    return thrd_success;

  // Compute deadline.
  DWORD InitialMs = timespecToRelativeMs(Ts);
  if (InitialMs == 0)
    return thrd_timedout;

  int64_t DeadlineNs = getMonotonicNs() + static_cast<int64_t>(InitialMs) * kNsPerMs;

  // Register as waiter.
  __c11_atomic_fetch_add(&Mtx->Waiters, 1, __ATOMIC_ACQ_REL);

  int Result = thrd_timedout;
  for (;;) {
    if (mtx_trylock(Mtx) == thrd_success) {
      Result = thrd_success;
      break;
    }

    DWORD Timeout = remainingMs(DeadlineNs);
    if (Timeout == 0)
      break;

    // Wait for unlock signal. WaitOnAddress wakes when Waiters changes,
    // which happens when another thread calls mtx_unlock.
    uint32_t Current = __c11_atomic_load(&Mtx->Waiters, __ATOMIC_ACQUIRE);
    WaitOnAddress(const_cast<_Atomic(uint32_t) *>(&Mtx->Waiters),
                  &Current, sizeof(uint32_t), Timeout);
  }

  __c11_atomic_fetch_sub(&Mtx->Waiters, 1, __ATOMIC_ACQ_REL);
  return Result;
}

int __cdecl _mtx_timedlock32(wincrt_mtx_t *Mtx, const Timespec32 *Ts) {
  WINCRT_ASSERT(Ts);
  Timespec64 Ts64 = {Ts->tv_sec, Ts->tv_nsec};
  return _mtx_timedlock64(Mtx, &Ts64);
}

int __cdecl mtx_timedlock(wincrt_mtx_t *Mtx, const Timespec64 *Ts) {
  return _mtx_timedlock64(Mtx, Ts);
}

} // extern "C"

// Condition Variable Functions (cnd_*)

extern "C" {

/// C11 7.26.3.2: Initializes condition variable.
int __cdecl cnd_init(wincrt_cnd_t *Cond) {
  WINCRT_ASSERT(Cond);
  InitializeConditionVariable(&Cond->Cv);
  return thrd_success;
}

/// C11 7.26.3.1: Destroys condition variable.
void __cdecl cnd_destroy(wincrt_cnd_t *Cond) {
  WINCRT_ASSERT(Cond);
  // CONDITION_VARIABLE has no destroy function.
  (void)Cond;
}

/// C11 7.26.3.4: Wakes one waiting thread.
int __cdecl cnd_signal(wincrt_cnd_t *Cond) {
  WINCRT_ASSERT(Cond);
  WakeConditionVariable(&Cond->Cv);
  return thrd_success;
}

/// C11 7.26.3.3: Wakes all waiting threads.
int __cdecl cnd_broadcast(wincrt_cnd_t *Cond) {
  WINCRT_ASSERT(Cond);
  WakeAllConditionVariable(&Cond->Cv);
  return thrd_success;
}

/// C11 7.26.3.6: Waits on condition variable.
int __cdecl cnd_wait(wincrt_cnd_t *Cond, wincrt_mtx_t *Mtx) {
  WINCRT_ASSERT(Cond && Mtx);

  BOOL Ok;
  switch (Mtx->Kind) {
  case kMtxCsRecursive:
  case kMtxCsTimedRecursive:
    Ok = SleepConditionVariableCS(&Cond->Cv, &Mtx->Cs, kInfinite);
    break;
  default:
    Ok = SleepConditionVariableSRW(&Cond->Cv, &Mtx->Srw, kInfinite,
                                   kCondVarLockExclusive);
    break;
  }
  return Ok ? thrd_success : thrd_error;
}

/// C11 7.26.3.5: Waits on condition variable with timeout.
int __cdecl _cnd_timedwait64(wincrt_cnd_t *Cond, wincrt_mtx_t *Mtx,
                              const Timespec64 *Ts) {
  WINCRT_ASSERT(Cond && Mtx);

  if (!isValidTimespec(Ts))
    return thrd_error;

  DWORD Timeout = timespecToRelativeMs(Ts);

  BOOL Ok;
  switch (Mtx->Kind) {
  case kMtxCsRecursive:
  case kMtxCsTimedRecursive:
    Ok = SleepConditionVariableCS(&Cond->Cv, &Mtx->Cs, Timeout);
    break;
  default:
    Ok = SleepConditionVariableSRW(&Cond->Cv, &Mtx->Srw, Timeout,
                                   kCondVarLockExclusive);
    break;
  }

  if (Ok)
    return thrd_success;
  if (GetLastError() == kErrorTimeout)
    return thrd_timedout;
  return thrd_error;
}

int __cdecl _cnd_timedwait32(wincrt_cnd_t *Cond, wincrt_mtx_t *Mtx,
                              const Timespec32 *Ts) {
  WINCRT_ASSERT(Ts);
  Timespec64 Ts64 = {Ts->tv_sec, Ts->tv_nsec};
  return _cnd_timedwait64(Cond, Mtx, &Ts64);
}

int __cdecl cnd_timedwait(wincrt_cnd_t *Cond, wincrt_mtx_t *Mtx,
                          const Timespec64 *Ts) {
  return _cnd_timedwait64(Cond, Mtx, Ts);
}

} // extern "C"

// Thread-Specific Storage (tss_*)
//
// C11 requires destructor callbacks on thread exit with iteration semantics.
// Windows TLS provides storage but no callbacks; we maintain a destructor
// table and dispatch from a TLS callback (.CRT$XLD).

namespace {

struct TssEntry {
  void (*Dtor)(void *);
  DWORD TlsIndex;
  bool InUse;
};

SRWLOCK g_TssLock;
TssEntry g_TssTable[kMaxTssKeys];
uint64_t g_TssInUseBitmap[(kMaxTssKeys + 63) / 64]; // For fast iteration

void runTssDestructors() {
  for (int Iter = 0; Iter < kTssDtorIterations; ++Iter) {
    bool AnyNonNull = false;

    // Snapshot under shared lock.
    TssEntry Snapshot[kMaxTssKeys];
    AcquireSRWLockShared(&g_TssLock);
    for (size_t I = 0; I < kMaxTssKeys; ++I)
      Snapshot[I] = g_TssTable[I];
    ReleaseSRWLockShared(&g_TssLock);

    // Process entries with destructors.
    for (size_t I = 0; I < kMaxTssKeys; ++I) {
      if (!Snapshot[I].InUse || !Snapshot[I].Dtor)
        continue;

      void *Val = TlsGetValue(Snapshot[I].TlsIndex);
      if (!Val)
        continue;

      AnyNonNull = true;

      // C11 7.26.6.1: Set to null before calling destructor.
      TlsSetValue(Snapshot[I].TlsIndex, nullptr);
      Snapshot[I].Dtor(Val);
    }

    if (!AnyNonNull)
      break;
  }
}

void __stdcall tssCleanupCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_THREAD_DETACH || Reason == DLL_PROCESS_DETACH)
    runTssDestructors();
}

} // namespace

#pragma section(".CRT$XLD", long, read)

extern "C" {

__declspec(allocate(".CRT$XLD")) PIMAGE_TLS_CALLBACK
    __wincrt_tss_cleanup = tssCleanupCallback;

#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:___wincrt_tss_cleanup")
#else
#pragma comment(linker, "/INCLUDE:__wincrt_tss_cleanup")
#endif

/// C11 7.26.6.1: Creates thread-specific storage key.
int __cdecl tss_create(wincrt_tss_t *Key, void (*Dtor)(void *)) {
  WINCRT_ASSERT(Key);

  DWORD TlsIdx = TlsAlloc();
  if (TlsIdx == kTlsOutOfIndexes)
    return thrd_error;

  AcquireSRWLockExclusive(&g_TssLock);

  size_t Slot = kMaxTssKeys;
  for (size_t I = 0; I < kMaxTssKeys; ++I) {
    if (!g_TssTable[I].InUse) {
      Slot = I;
      break;
    }
  }

  if (Slot == kMaxTssKeys) {
    ReleaseSRWLockExclusive(&g_TssLock);
    TlsFree(TlsIdx);
    return thrd_error;
  }

  g_TssTable[Slot].TlsIndex = TlsIdx;
  g_TssTable[Slot].Dtor = Dtor;
  g_TssTable[Slot].InUse = true;
  g_TssInUseBitmap[Slot / 64] |= 1ULL << (Slot % 64);

  ReleaseSRWLockExclusive(&g_TssLock);

  Key->Key = TlsIdx;
  return thrd_success;
}

/// C11 7.26.6.2: Deletes thread-specific storage key.
void __cdecl tss_delete(wincrt_tss_t Key) {
  AcquireSRWLockExclusive(&g_TssLock);

  for (size_t I = 0; I < kMaxTssKeys; ++I) {
    if (g_TssTable[I].InUse && g_TssTable[I].TlsIndex == Key.Key) {
      g_TssTable[I].InUse = false;
      g_TssTable[I].Dtor = nullptr;
      g_TssInUseBitmap[I / 64] &= ~(1ULL << (I % 64));
      break;
    }
  }

  ReleaseSRWLockExclusive(&g_TssLock);
  TlsFree(Key.Key);
}

/// C11 7.26.6.3: Gets thread-specific storage value.
void *__cdecl tss_get(wincrt_tss_t Key) {
  return TlsGetValue(Key.Key);
}

/// C11 7.26.6.4: Sets thread-specific storage value.
int __cdecl tss_set(wincrt_tss_t Key, void *Val) {
  return TlsSetValue(Key.Key, Val) ? thrd_success : thrd_error;
}

} // extern "C"

// Call Once (call_once)

namespace {

struct CallOnceContext {
  void (*Func)(void);
};

BOOL __stdcall callOnceCallback(PINIT_ONCE, void *Param, void **) {
  static_cast<CallOnceContext *>(Param)->Func();
  return TRUE;
}

} // namespace

extern "C" {

/// C11 7.26.2.1: Calls function exactly once.
void __cdecl call_once(wincrt_once_flag *Flag, void (*Func)(void)) {
  WINCRT_ASSERT(Flag && Func);
  CallOnceContext Ctx = {Func};
  InitOnceExecuteOnce(&Flag->Once, callOnceCallback, &Ctx, nullptr);
}

} // extern "C"

// Module Initialization

namespace {

int __cdecl initC11Threads() {
  // Initialize FLS slot early.
  (void)getThreadStateFls();

  // Initialize interrupt map.
  g_InterruptMap.init();

  // Set up main thread state.
  ThreadState *State = getOrCreateThreadState();
  if (State) {
    State->Tid = GetCurrentThreadId();
    g_InterruptMap.insert(State->Tid, State);
  }

  return 0;
}

} // namespace

#pragma section(".CRT$XIU", long, read)

extern "C" {

__declspec(allocate(".CRT$XIU")) _PIFV __wincrt_c11threads_init = initC11Threads;

#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:___wincrt_c11threads_init")
#else
#pragma comment(linker, "/INCLUDE:__wincrt_c11threads_init")
#endif

} // extern "C"

#endif // _WIN32
