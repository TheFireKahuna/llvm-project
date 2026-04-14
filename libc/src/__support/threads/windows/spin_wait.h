//===--- Hardware-adaptive spin wait for Windows ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Adaptive spin before kernel sleep. Three tiers:
//   1. UMONITOR/UMWAIT  (Intel 12th gen+) — hardware address monitor
//   2. MONITORX/MWAITX  (AMD Zen+)        — hardware address monitor
//   3. Arch-local relax hint              — PAUSE on x86, YIELD on AArch64
//
// The spin budget comes from KUSER_SHARED_DATA, the same source ntdll uses
// for its adaptive spin in RtlWaitOnAddress. CPUID detection runs once.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SPIN_WAIT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SPIN_WAIT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_word.h"

namespace LIBC_NAMESPACE_DECL {
namespace spin_wait {

enum class SpinBackend : uint8_t {
  RELAX,    // Architecture-local fallback
  UMWAIT,   // Intel WAITPKG (CPUID.7.0:ECX bit 5)
  MWAITX,   // AMD (CPUID.80000001:ECX bit 29)
};

// KUSER_SHARED_DATA is mapped read-only at a fixed VA in every process.
// Offset 0x036A: spin count threshold — non-zero enables adaptive spinning.
LIBC_INLINE uint16_t kuser_spin_threshold() {
  return *reinterpret_cast<const volatile uint16_t *>(0x7FFE036Aull);
}

// Per-architecture spin hint used by the fallback path when no address-monitor
// backend is available.
LIBC_INLINE void relax_processor() {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#ifdef __has_builtin
#if __has_builtin(__builtin_arm_yield)
  __builtin_arm_yield();
  return;
#endif
#endif
  __asm__ volatile("yield" ::: "memory");
#else
  __asm__ volatile("" ::: "memory");
#endif
}

// Detect hardware spin capability. Called once; result cached.
LIBC_INLINE SpinBackend detect_backend() {
#if defined(__x86_64__) || defined(_M_X64)
  uint32_t eax, ebx, ecx, edx;

  // Intel WAITPKG: CPUID leaf 7, subleaf 0, ECX bit 5
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(7), "c"(0));
  if (ecx & (1u << 5))
    return SpinBackend::UMWAIT;

  // AMD MONITORX: CPUID leaf 0x80000001, ECX bit 29
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(0x80000001), "c"(0));
  if (ecx & (1u << 29))
    return SpinBackend::MWAITX;
#endif
  return SpinBackend::RELAX;
}

// Cached backend — initialized on first use (no dynamic init needed).
LIBC_INLINE SpinBackend get_backend() {
  static SpinBackend backend = detect_backend();
  return backend;
}

// Spin on a futex value until it differs from `expected`. Returns true if
// the value changed, false if the spin budget was exhausted.
// Monitors the futex word directly — any store to the cache line wakes
// the hardware monitor immediately, unlike ntdll which spins on node.State.
LIBC_INLINE bool spin_until_changed(cpp::Atomic<FutexWordType> *futex,
                                    FutexWordType expected) {
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false; // OS disabled adaptive spinning

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &futex->val;

  if (backend == SpinBackend::UMWAIT) {
    uint64_t deadline = __builtin_ia32_rdtsc() + threshold * 1024;
    for (;;) {
      // Arm address monitor on the futex cache line.
      __asm__ volatile("umonitor %0" : : "r"(addr) : "memory");
      FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
      if (cur != expected)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      // C0.2 (deeper sleep), bounded by TSC deadline.
      uint32_t lo = static_cast<uint32_t>(deadline);
      uint32_t hi = static_cast<uint32_t>(deadline >> 32);
      // umwait: eax=1 (C0.2), edx:eax = TSC deadline
      uint8_t cf;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(cf)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
    }
  }

  if (backend == SpinBackend::MWAITX) {
    uint64_t deadline = __builtin_ia32_rdtsc() + threshold * 1024;
    for (;;) {
      // monitorx addr, ecx=0, edx=0
      __asm__ volatile("monitorx" : : "a"(addr), "c"(0), "d"(0) : "memory");
      FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
      if (cur != expected)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint64_t remaining = deadline - now;
      uint32_t ticks = remaining > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                : static_cast<uint32_t>(remaining);
      // mwaitx: ecx=2 (use ebx as timer), ebx=TSC ticks
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#endif

  // Architecture-local fallback: PAUSE on x86, YIELD on AArch64.
  for (uint16_t i = 0; i < threshold; ++i) {
    FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
    if (cur != expected)
      return true;
    relax_processor();
  }
  return false;
}

// Spin on a raw uint32_t pointer until it differs from `expected`. Returns
// true if the value changed, false if the spin budget was exhausted.
//
// Same UMWAIT/MWAITX monitoring as spin_until_changed(Atomic*), but takes
// a raw pointer — suitable for kernel-mapped shared memory (IoRing CQ tail,
// NT event words, etc.) that isn't wrapped in cpp::Atomic.
//
// `tsc_multiplier` controls the budget: threshold × multiplier TSC ticks.
// Default 1024 matches spin_until_changed. IoRing callers may use higher
// values (e.g. 4096 ≈ 14μs) to catch NVMe completions without entering
// the parking lot.
// Template version to avoid ULONG/uint32_t type mismatch on Windows
// (unsigned long vs unsigned int, both 32-bit).
template <typename T>
LIBC_INLINE bool spin_on_raw_u32(volatile T *addr, T expected,
                                 uint32_t tsc_multiplier = 1024) {
  static_assert(sizeof(T) == 4, "spin_on_raw_u32 requires 32-bit type");
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false;

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  if (backend == SpinBackend::UMWAIT) {
    uint64_t deadline = __builtin_ia32_rdtsc() +
                        static_cast<uint64_t>(threshold) * tsc_multiplier;
    for (;;) {
      __asm__ volatile("umonitor %0"
                       : : "r"(const_cast<T *>(addr)) : "memory");
      T cur = __atomic_load_n(addr, __ATOMIC_RELAXED);
      if (cur != expected)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint32_t lo = static_cast<uint32_t>(deadline);
      uint32_t hi = static_cast<uint32_t>(deadline >> 32);
      uint8_t cf;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(cf)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
    }
  }

  if (backend == SpinBackend::MWAITX) {
    uint64_t deadline = __builtin_ia32_rdtsc() +
                        static_cast<uint64_t>(threshold) * tsc_multiplier;
    for (;;) {
      __asm__ volatile("monitorx"
                       : : "a"(const_cast<T *>(addr)),
                           "c"(0), "d"(0)
                       : "memory");
      T cur = __atomic_load_n(addr, __ATOMIC_RELAXED);
      if (cur != expected)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint64_t remaining = deadline - now;
      uint32_t ticks = remaining > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                : static_cast<uint32_t>(remaining);
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#endif

  // PAUSE fallback: limited iterations (PAUSE consumes SMT resources).
  uint16_t count = threshold < 64 ? threshold : 64;
  for (uint16_t i = 0; i < count; ++i) {
    T cur = __atomic_load_n(addr, __ATOMIC_RELAXED);
    if (cur != expected)
      return true;
    relax_processor();
  }
  return false;
}

// Brief pre-kernel spin on a slot state byte. Monitors the slot's cache
// line with UMWAIT/MWAITX; returns true if `*state` changed from
// `expected_state` within the budget, false on timeout.
//
// Used between wait-list insert and NtWaitForAlertByThreadId: the thread
// is in WAITING state and visible to wakers. If a handoff CAS
// (WAITING→SIGNALED) fires during this window, the hardware monitor
// wakes immediately — zero-syscall wake. UMWAIT C0.2 is near-zero
// power, so it doesn't steal resources from SMT siblings.
//
// Budget: threshold × 256 TSC ticks. With kuser_spin_threshold=16
// (common on AMD), this gives ~900ns at 4.5GHz. UMWAIT C0.2 and MWAITX
// are cache-line monitors: the CPU sleeps until the target line is
// written, so this budget is just a ceiling, not busy-wait.
//
// This budget is a performance knob, NOT a correctness dependency.
// handoff_one_for only targets WAITING slots (this spin phase).
// If the spin expires before the handoff CAS arrives, the thread
// transitions to IN_KERNEL, handoff_one_for skips it, and the caller
// falls back to the Dekker-protocol store_and_notify(0) path.
// A longer budget means more handoffs are caught in user-mode
// (faster), a shorter budget means more fall through to IN_KERNEL
// (correct but slower due to kernel round-trip).
LIBC_INLINE bool spin_on_slot_state(cpp::Atomic<uint8_t> *state,
                                    uint8_t expected_state) {
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false;

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &state->val;

  if (backend == SpinBackend::UMWAIT) {
    uint64_t deadline = __builtin_ia32_rdtsc() +
                        static_cast<uint64_t>(threshold) * 256;
    for (;;) {
      __asm__ volatile("umonitor %0" : : "r"(addr) : "memory");
      if (state->load(cpp::MemoryOrder::RELAXED) != expected_state)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint32_t lo = static_cast<uint32_t>(deadline);
      uint32_t hi = static_cast<uint32_t>(deadline >> 32);
      uint8_t cf;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(cf)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
    }
  }

  if (backend == SpinBackend::MWAITX) {
    uint64_t deadline = __builtin_ia32_rdtsc() +
                        static_cast<uint64_t>(threshold) * 256;
    for (;;) {
      __asm__ volatile("monitorx" : : "a"(addr), "c"(0), "d"(0) : "memory");
      if (state->load(cpp::MemoryOrder::RELAXED) != expected_state)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint64_t remaining = deadline - now;
      uint32_t ticks = remaining > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                : static_cast<uint32_t>(remaining);
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#endif

  // No PAUSE fallback: without a hardware address monitor, PAUSE spin
  // consumes SMT execution resources. Under high contention (16+ threads)
  // multiple threads spin simultaneously, starving the lock holder. Only
  // UMWAIT C0.2 / MWAITX are truly low-power enough for this spin phase.
  (void)expected_state;
  return state->load(cpp::MemoryOrder::RELAXED) != expected_state;
}

} // namespace spin_wait
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SPIN_WAIT_H
