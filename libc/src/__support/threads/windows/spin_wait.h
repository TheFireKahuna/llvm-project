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
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_word.h"

namespace LIBC_NAMESPACE_DECL {
namespace spin_wait {

enum class SpinBackend : uint8_t {
  RELAX,    // Architecture-local fallback
  UMWAIT,   // Intel WAITPKG (CPUID.7.0:ECX bit 5)
  MWAITX,   // AMD (CPUID.80000001:ECX bit 29)
  WFET,     // AArch64 v8.7+: LDXR + WFET (FEAT_WFxT, abs. CNTVCT deadline)
  WFE,      // AArch64 baseline: LDXR + WFE (event-stream / line-write wake)
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

#if defined(__aarch64__) || defined(_M_ARM64)
namespace aarch64 {

// CNTVCT_EL0 — architectural virtual counter. Frequency in CNTFRQ_EL0;
// 24MHz (~41.7ns/tick) on Apple Silicon, most Cortex-A, Graviton 2/3.
// Some server SoCs (Ampere Altra, certain Graviton SKUs) run at 100MHz.
// Read cost is comparable to RDTSC — non-trapping mrs.
LIBC_INLINE uint64_t read_cntvct() {
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

// LDXR family — width-dispatched. Arms the local exclusive monitor on
// the granule containing addr (granule size = CTR_EL0.ERG, typically
// 64 or 128B — same false-wake hazard as UMONITOR's cache line).
// "Q" forces base-register addressing; LDXR rejects offset forms.
template <typename T>
LIBC_INLINE T ldxr_load(volatile T *addr) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 ||
                sizeof(T) == 4 || sizeof(T) == 8,
                "ldxr_load: requires 1/2/4/8-byte scalar");
  if constexpr (sizeof(T) == 1) {
    uint32_t v;
    __asm__ volatile("ldxrb %w0, %1" : "=r"(v) : "Q"(*addr));
    return static_cast<T>(v);
  } else if constexpr (sizeof(T) == 2) {
    uint32_t v;
    __asm__ volatile("ldxrh %w0, %1" : "=r"(v) : "Q"(*addr));
    return static_cast<T>(v);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t v;
    __asm__ volatile("ldxr %w0, %1" : "=r"(v) : "Q"(*addr));
    return static_cast<T>(v);
  } else {
    uint64_t v;
    __asm__ volatile("ldxr %0, %1" : "=r"(v) : "Q"(*addr));
    return static_cast<T>(v);
  }
}

// CLREX — release the local monitor without storing. Emitted on
// early-exit paths so a stale arm doesn't bleed into caller code
// where an unrelated cache-line write could trigger spurious wakes.
LIBC_INLINE void clrex() {
  __asm__ volatile("clrex" ::: "memory");
}

LIBC_INLINE void wfe() {
  __asm__ volatile("wfe" ::: "memory");
}

// WFET Xn — Wait For Event with Timeout. FEAT_WFxT (ARMv8.7+).
// Xn = absolute CNTVCT_EL0 deadline. Wake conditions: monitor cleared
// (any PE wrote the granule), SEV from another PE, async exception,
// event-stream tick, or counter ≥ Xn. .arch_extension scopes the
// encoding requirement to this asm block — no -march bump TU-wide.
// Runtime-gated on ARM_WFXT, so non-WFxT hosts never reach this insn.
LIBC_INLINE void wfet(uint64_t deadline) {
  __asm__ volatile(".arch_extension wfxt\n\t"
                   "wfet %0\n\t"
                   ".arch_extension nowfxt"
                   :: "r"(deadline) : "memory");
}

// Convert an x86-side TSC budget shift to a CNTVCT-equivalent shift
// targeting the same wall-time. Calibrated for CNTFRQ=24MHz vs.
// nominal TSC=3.0-4.5GHz: ~128× ratio → shift down by 7. Server SoCs
// at 100MHz CNTFRQ over-budget by ~4× — acceptable for a spin ceiling
// since the line-write wake is exact regardless.
inline constexpr unsigned kCntvctShiftFromTsc = 7;

} // namespace aarch64
#endif

// Resolve the hardware spin backend. CPUID runs once at Phase 0 (see
// probe_nt_capabilities); the result is cached in the sealed PcbZone0
// capability bitmask. This accessor compiles to a single 4-byte load
// from the PCB page plus two AND-tests — no function-local-static guard,
// no atomic acquire, no indirect call. Branch predictor pins the outcome
// after the first call in any given spin function.
LIBC_INLINE SpinBackend get_backend() {
#if defined(__x86_64__) || defined(_M_X64)
  uint32_t caps = nt_caps_fast();
  if (caps & nt_cap::X86_UMWAIT)
    return SpinBackend::UMWAIT;
  if (caps & nt_cap::X86_MWAITX)
    return SpinBackend::MWAITX;
  return SpinBackend::RELAX;
#elif defined(__aarch64__) || defined(_M_ARM64)
  uint32_t caps = nt_caps_fast();
  // ARM_WFXT: ID_AA64ISAR2_EL1.WFxT >= 0b0010 (probed at Phase 0
  // via IsProcessorFeaturePresent on Windows, or direct ID-reg
  // read where the OS exposes it). Add to nt_capabilities.h.
  if (caps & nt_cap::ARM_WFXT)
    return SpinBackend::WFET;
  return SpinBackend::WFE;
#else
  return SpinBackend::RELAX;
#endif
}

// Spin on a futex value until it differs from `expected`. Returns true if
// the value changed, false if the spin budget was exhausted.
// Monitors the futex word directly — any store to the cache line wakes
// the hardware monitor immediately, unlike ntdll which spins on node.State.
//
// target("waitpkg,mwaitx") enables the __builtin_ia32_{umonitor,umwait,
// monitorx,mwaitx} builtins regardless of the translation unit's default
// target CPU. Runtime dispatch through get_backend() still gates which
// branch actually executes on this host.
#if defined(__x86_64__) || defined(_M_X64)
[[gnu::target("waitpkg,mwaitx")]]
#endif
LIBC_INLINE bool spin_until_changed(cpp::Atomic<FutexWordType> *futex,
                                    FutexWordType expected) {
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false; // OS disabled adaptive spinning

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &futex->val;

  if (backend == SpinBackend::UMWAIT) {
    // Early-exit: value already differs. Skips the full UMONITOR + UMWAIT
    // round-trip (monitor arm + C0.2 entry/exit) on the common contended
    // path where the racing store landed before we entered the spin
    // (mutex release winning the CAS just ahead of us). Cost in the slow
    // path is one relaxed load.
    if (futex->load(cpp::MemoryOrder::RELAXED) != expected)
      return true;

    // UMWAIT takes an absolute TSC deadline in edx:eax — loop-invariant,
    // so compute the split once outside the loop. The builtin returns CF
    // directly: 1 on deadline timeout, 0 on monitor/interrupt wake. That
    // lets us skip the per-iteration RDTSC + compare entirely
    // (~20-30 cycles/iteration on Skylake+).
    uint64_t deadline = __builtin_ia32_rdtsc() +
                        (static_cast<uint64_t>(threshold) << 10);
    uint32_t lo = static_cast<uint32_t>(deadline);
    uint32_t hi = static_cast<uint32_t>(deadline >> 32);

    for (;;) {
      // Canonical ordering: arm monitor, THEN re-load to close the race
      // where the line is written between check and arm.
      __builtin_ia32_umonitor(addr);
      FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
      if (cur != expected)
        return true;

      // UMWAIT ctrl=1 selects C0.2 (deeper sleep). Builtin signature:
      // (ctrl, hi, lo) → CF. Any non-timeout wake returns CF=0; we loop
      // and let the next load catch the write.
      if (__builtin_ia32_umwait(1u, hi, lo))
        return false;
    }
  }

  if (backend == SpinBackend::MWAITX) {
    // MWAITX's EBX is a *relative* TSC tick count, and AMD exposes no
    // flag distinguishing timeout from event — so we must RDTSC after
    // every wake to narrow the remaining budget. Peel iter 1 to avoid
    // a runtime branch on "first pass" state.
    //
    // threshold is u16, so budget = threshold << 10 ≤ 2^26 — fits u32
    // with 64× headroom. (deadline - now) is bounded by budget while the
    // TSC is monotonic, so narrowing to u32 below is clamp-free.
    // A pathological backward TSC glitch would only truncate the next
    // wait, not break correctness: the outer `now >= deadline` check
    // still terminates the loop.
    //
    // __builtin_ia32_mwaitx(a, b, c) maps to physregs (ecx, eax, ebx)
    // per LLVM's X86 binding — NOT the (extensions, hints, clock) the
    // clang header comment implies. We pass ecx=2 (bit 1 = use EBX as
    // TSC timer), eax=0 (C0 default power state), ebx=ticks.
    const uint32_t budget = static_cast<uint32_t>(threshold) << 10;
    const uint64_t deadline = __builtin_ia32_rdtsc() + budget;

    // --- Peeled iter 1: full budget, no RDTSC needed ---
    __builtin_ia32_monitorx(addr, 0, 0);
    FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
    if (cur != expected)
      return true;
    __builtin_ia32_mwaitx(2u, 0u, budget);

    // --- Steady-state iters: re-arm, check, measure, sleep ---
    for (;;) {
      __builtin_ia32_monitorx(addr, 0, 0);
      cur = futex->load(cpp::MemoryOrder::RELAXED);
      if (cur != expected)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      __builtin_ia32_mwaitx(2u, 0u, static_cast<uint32_t>(deadline - now));
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (backend == SpinBackend::WFET) {
    // No early-exit needed: LDXR fuses the arm + load. The first
    // iteration's compare catches a pre-spin store with the same
    // efficiency as the x86 early-exit pattern.
    uint64_t deadline = aarch64::read_cntvct() +
        (static_cast<uint64_t>(threshold)
         << (10 - aarch64::kCntvctShiftFromTsc));

    for (;;) {
      FutexWordType cur = aarch64::ldxr_load(&futex->val);
      if (cur != expected) {
        aarch64::clrex();
        return true;
      }
      aarch64::wfet(deadline);
      // WFET reports neither timeout nor wake cause; the counter
      // re-read distinguishes. Spurious wakes (writes that don't
      // satisfy the predicate) cost one extra LDXR + cmp + WFET.
      if (aarch64::read_cntvct() >= deadline)
        return false;
    }
  }

  if (backend == SpinBackend::WFE) {
    // Identical skeleton — WFE without timeout argument. Wakes
    // come from line writes, cross-PE SEV, async exceptions, or
    // event-stream ticks (CNTKCTL_EL1.EVNTEN, programmed by the
    // kernel for userspace; default period ~100μs on Linux,
    // similar on Windows ARM64). The deadline check after each
    // wake bounds total spin time the same way as WFET, just at
    // event-stream granularity.
    uint64_t deadline = aarch64::read_cntvct() +
        (static_cast<uint64_t>(threshold)
         << (10 - aarch64::kCntvctShiftFromTsc));

    for (;;) {
      FutexWordType cur = aarch64::ldxr_load(&futex->val);
      if (cur != expected) {
        aarch64::clrex();
        return true;
      }
      aarch64::wfe();
      if (aarch64::read_cntvct() >= deadline)
        return false;
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

// Predicate-aware companion of spin_until_changed. Spins on the cache
// line via UMWAIT / MWAITX, re-evaluating `pred(load(), arg)` on every
// wake. Returns true iff pred became TRUE within the spin budget,
// false on timeout.
//
// Semantic difference vs. spin_until_changed: a value change that does
// NOT flip the predicate re-arms the monitor rather than returning. This
// eliminates the "top-level absorb round-trip" under thundering-herd
// contention where the winner flips value_ but the predicate stays
// FALSE for losers — each loser stays in C0.2 instead of bouncing
// through Phase 0 and re-entering Phase 1 N times.
//
// Predicate contract matches wait_slot::PredicateFn: noexcept, side-
// effect free, no locks, no allocation. The waker invokes the same
// function; any non-determinism would flip waiter/waker verdicts.
#if defined(__x86_64__) || defined(_M_X64)
[[gnu::target("waitpkg,mwaitx")]]
#endif
LIBC_INLINE bool spin_on_pred(cpp::Atomic<FutexWordType> *futex,
                              bool (*pred)(uint32_t, uint32_t) noexcept,
                              uint32_t arg) {
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false; // OS disabled adaptive spinning

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &futex->val;

  if (backend == SpinBackend::UMWAIT) {
    // Early-exit: predicate already holds. Skips the UMONITOR + UMWAIT
    // round-trip when the racing store (and predicate flip) landed before
    // we entered the spin.
    if (pred(futex->load(cpp::MemoryOrder::RELAXED), arg))
      return true;

    uint64_t deadline = __builtin_ia32_rdtsc() +
                        (static_cast<uint64_t>(threshold) << 10);
    uint32_t lo = static_cast<uint32_t>(deadline);
    uint32_t hi = static_cast<uint32_t>(deadline >> 32);

    for (;;) {
      __builtin_ia32_umonitor(addr);
      FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
      if (pred(cur, arg))
        return true;

      // UMWAIT ctrl=1 selects C0.2 (deeper sleep). On any non-timeout
      // wake the builtin returns CF=0; we loop and re-check the
      // predicate against the now-settled value. Spurious wakes
      // (writes that don't flip pred) cost one extra umonitor+load+
      // compare — no syscall.
      if (__builtin_ia32_umwait(1u, hi, lo))
        return false;
    }
  }

  if (backend == SpinBackend::MWAITX) {
    const uint32_t budget = static_cast<uint32_t>(threshold) << 10;
    const uint64_t deadline = __builtin_ia32_rdtsc() + budget;

    // --- Peeled iter 1: full budget, no RDTSC needed ---
    __builtin_ia32_monitorx(addr, 0, 0);
    FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
    if (pred(cur, arg))
      return true;
    __builtin_ia32_mwaitx(2u, 0u, budget);

    // --- Steady-state iters: re-arm, check, measure, sleep ---
    for (;;) {
      __builtin_ia32_monitorx(addr, 0, 0);
      cur = futex->load(cpp::MemoryOrder::RELAXED);
      if (pred(cur, arg))
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      __builtin_ia32_mwaitx(2u, 0u, static_cast<uint32_t>(deadline - now));
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (backend == SpinBackend::WFET || backend == SpinBackend::WFE) {
    uint64_t deadline = aarch64::read_cntvct() +
        (static_cast<uint64_t>(threshold)
         << (10 - aarch64::kCntvctShiftFromTsc));

    for (;;) {
      FutexWordType cur = aarch64::ldxr_load(&futex->val);
      if (pred(cur, arg)) {
        aarch64::clrex();
        return true;
      }
      if (backend == SpinBackend::WFET)
        aarch64::wfet(deadline);
      else
        aarch64::wfe();
      if (aarch64::read_cntvct() >= deadline)
        return false;
    }
  }
#endif

  // Architecture-local fallback: PAUSE on x86, YIELD on AArch64.
  for (uint16_t i = 0; i < threshold; ++i) {
    FutexWordType cur = futex->load(cpp::MemoryOrder::RELAXED);
    if (pred(cur, arg))
      return true;
    relax_processor();
  }
  return false;
}

// Spin on a raw scalar pointer until it differs from `expected`. Returns
// true if the value changed, false if the spin budget was exhausted.
//
// Same UMWAIT/MWAITX monitoring as spin_until_changed(Atomic*), but takes
// a raw pointer — suitable for kernel-mapped shared memory (IoRing CQ tail,
// NT event words, etc.) that isn't wrapped in cpp::Atomic, and for the
// futex_addr parking lot's Phase 1 value spin over arbitrary 1/2/4/8 byte
// futex words. UMONITOR/MWAITX monitor at cache-line granularity, so any
// scalar width works — the CPU sleeps until the target line is written.
//
// `addr` is non-volatile by design: the loads use `__atomic_load_n` (an
// opaque compiler intrinsic — never reordered or hoisted), and the
// inline asm uses `asm volatile` for ordering. A `volatile T *`
// parameter would require callers to const_cast at every call site
// when T itself is a pointer (C++ qualification rule forbids implicit
// `T**` → `T * volatile *`), and would gain nothing — the volatile
// would be const_cast away inside the asm constraints regardless.
//
// `tsc_multiplier` controls the budget: threshold × multiplier TSC ticks.
// Default 1024 matches spin_until_changed. IoRing callers may use higher
// values (e.g. 4096 ≈ 14μs) to catch NVMe completions without entering
// the parking lot.
template <typename T>
LIBC_INLINE bool spin_on_raw(T *addr, T expected,
                             uint32_t tsc_multiplier = 1024) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 ||
                sizeof(T) == 4 || sizeof(T) == 8,
                "spin_on_raw requires 1/2/4/8-byte scalar type");
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false;

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  if (backend == SpinBackend::UMWAIT) {
    // Early-exit: value already differs. See spin_until_changed for
    // rationale — saves the UMONITOR + UMWAIT round-trip on contended
    // paths where the store landed before we entered the spin.
    if (__atomic_load_n(addr, __ATOMIC_RELAXED) != expected)
      return true;

    // See spin_until_changed for the rationale on hoisted lo/hi +
    // CF-based termination.
    uint64_t deadline = __builtin_ia32_rdtsc() +
                        static_cast<uint64_t>(threshold) * tsc_multiplier;
    uint32_t lo = static_cast<uint32_t>(deadline);
    uint32_t hi = static_cast<uint32_t>(deadline >> 32);

    for (;;) {
      // UMONITOR only reads *addr — declare the dependency via "m"(*addr)
      // instead of a full "memory" clobber. The compiler still can't hoist
      // the subsequent load of *addr above it, but caller-side state no
      // longer gets force-spilled. UMWAIT below keeps its "memory" clobber
      // because it sleeps and any memory can change during the wait.
      __asm__ volatile("umonitor %0"
                       :
                       : "r"(addr), "m"(*addr)
                       :);
      T cur = __atomic_load_n(addr, __ATOMIC_RELAXED);
      if (cur != expected)
        return true;
      uint8_t timed_out;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(timed_out)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
      if (timed_out)
        return false;
    }
  }

  if (backend == SpinBackend::MWAITX) {
    uint64_t budget = static_cast<uint64_t>(threshold) * tsc_multiplier;
    uint64_t deadline = __builtin_ia32_rdtsc() + budget;
    uint32_t ticks = budget > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                           : static_cast<uint32_t>(budget);

    // MONITORX reads *addr only — narrow "memory" clobber to "m"(*addr)
    // input. MWAITX keeps "memory" since it sleeps and any memory can
    // change across the wait.
    // --- Peeled iter 1: full budget, no RDTSC needed ---
    __asm__ volatile("monitorx"
                     :
                     : "a"(addr), "c"(0), "d"(0), "m"(*addr)
                     :);
    T cur = __atomic_load_n(addr, __ATOMIC_RELAXED);
    if (cur != expected)
      return true;
    __asm__ volatile("mwaitx"
                     : : "a"(0), "c"(2), "b"(ticks)
                     : "memory");

    // --- Steady-state iters ---
    for (;;) {
      __asm__ volatile("monitorx"
                       :
                       : "a"(addr), "c"(0), "d"(0), "m"(*addr)
                       :);
      cur = __atomic_load_n(addr, __ATOMIC_RELAXED);
      if (cur != expected)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint64_t remaining = deadline - now;
      ticks = remaining > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                       : static_cast<uint32_t>(remaining);
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (backend == SpinBackend::WFET || backend == SpinBackend::WFE) {
    uint64_t budget = static_cast<uint64_t>(threshold) *
                      (tsc_multiplier >> aarch64::kCntvctShiftFromTsc);
    if (budget == 0)
      budget = 1;  // floor: tsc_multiplier < 128 still gets one tick
    uint64_t deadline = aarch64::read_cntvct() + budget;

    for (;;) {
      T cur = aarch64::ldxr_load(addr);
      if (cur != expected) {
        aarch64::clrex();
        return true;
      }
      if (backend == SpinBackend::WFET)
        aarch64::wfet(deadline);
      else
        aarch64::wfe();
      if (aarch64::read_cntvct() >= deadline)
        return false;
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

// Brief pre-kernel spin on a state byte. Monitors the owning cache
// line with UMWAIT/MWAITX; returns true if the state byte changed
// from `expected_state` within the budget, false on timeout.
//
// Used between wait-list insert and NtWaitForAlertByThreadId: the
// thread is in WAITING state and visible to wakers. If a handoff CAS
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
// handoff_one only targets WAITING slots (this spin phase). If the
// spin expires before the handoff CAS arrives, the thread transitions
// to IN_KERNEL, handoff_one skips it, and the caller falls back to
// the Dekker-protocol store_and_notify(0) path. A longer budget
// means more handoffs are caught in user-mode (faster), a shorter
// budget means more fall through to IN_KERNEL (correct but slower).
//
// Shared implementation with per-width specialization via `if
// constexpr`:
//   - `Atomic<uint8_t>` (InitLatch, indexed_pool byte states): the
//     whole word IS the state — identity load, no extraction.
//   - `Atomic<uint64_t>` (WaitSlot.link = [state:8 | reserved:8 |
//     tag:32 | next:16]): state is the high byte. On little-endian
//     x86-64, we byte-load directly from offset 7 of the word —
//     saves a shift compared to "full 64-bit load + (v >> 56)". This
//     matters per spin iteration of the asm loop.
//
// The `extract_state` helper is the only per-width divergence; the
// UMWAIT / MWAITX skeleton is otherwise identical.
namespace detail {

template <typename Word>
LIBC_INLINE uint8_t extract_state(cpp::Atomic<Word> *word) {
  if constexpr (sizeof(Word) == 1) {
    // Identity — the atomic word IS the state byte.
    return word->load(cpp::MemoryOrder::RELAXED);
  } else {
    static_assert(sizeof(Word) == 8,
                  "spin_on_state_impl: only 1-byte or 8-byte atomics");
    // LE x86-64: state byte is the high byte of the 8-byte word at
    // byte offset 7. Byte-load avoids the full-word load + shift that
    // a naive "(load() >> 56)" would emit. Still reads the same cache
    // line the UMONITOR is watching, so semantics are identical.
    return reinterpret_cast<volatile uint8_t *>(&word->val)[7];
  }
}

template <typename Word>
LIBC_INLINE bool spin_on_state_impl(cpp::Atomic<Word> *word,
                                     uint8_t expected_state) {
  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return false;

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &word->val;

  if (backend == SpinBackend::UMWAIT) {
    // Early-exit: already changed. Skips UMONITOR + UMWAIT when the
    // handoff CAS landed just before we entered the spin.
    if (extract_state(word) != expected_state)
      return true;

    uint64_t deadline = __builtin_ia32_rdtsc() +
                        (static_cast<uint64_t>(threshold) << 8);
    uint32_t lo = static_cast<uint32_t>(deadline);
    uint32_t hi = static_cast<uint32_t>(deadline >> 32);

    for (;;) {
      // UMONITOR only reads word->val — narrow "memory" clobber to an
      // "m" input declaring the dependency. UMWAIT keeps "memory".
      __asm__ volatile("umonitor %0"
                       : : "r"(addr), "m"(word->val) :);
      if (extract_state(word) != expected_state)
        return true;
      uint8_t timed_out;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(timed_out)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
      if (timed_out)
        return false;
    }
  }

  if (backend == SpinBackend::MWAITX) {
    // threshold (u16) << 8 ≤ 2^24 fits u32 with ~256× headroom, so the
    // narrowing (deadline - now) cast below is clamp-free. See
    // spin_until_changed for the full rationale.
    const uint32_t budget = static_cast<uint32_t>(threshold) << 8;
    const uint64_t deadline = __builtin_ia32_rdtsc() + budget;

    // MONITORX reads word->val only — narrow clobber. MWAITX keeps
    // "memory" since it sleeps.
    // --- Peeled iter 1 ---
    __asm__ volatile("monitorx"
                     : : "a"(addr), "c"(0), "d"(0), "m"(word->val) :);
    if (extract_state(word) != expected_state)
      return true;
    __asm__ volatile("mwaitx"
                     : : "a"(0), "c"(2), "b"(budget)
                     : "memory");

    // --- Steady-state iters ---
    for (;;) {
      __asm__ volatile("monitorx"
                       : : "a"(addr), "c"(0), "d"(0), "m"(word->val) :);
      if (extract_state(word) != expected_state)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint32_t ticks = static_cast<uint32_t>(deadline - now);
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (backend == SpinBackend::WFET || backend == SpinBackend::WFE) {
    uint64_t deadline = aarch64::read_cntvct() +
        (static_cast<uint64_t>(threshold)
         << (8 - aarch64::kCntvctShiftFromTsc > 0
                ? 8 - aarch64::kCntvctShiftFromTsc : 1));

    for (;;) {
      uint8_t state;
      if constexpr (sizeof(Word) == 1) {
        state = aarch64::ldxr_load(
            reinterpret_cast<volatile uint8_t *>(&word->val));
      } else {
        // LDXR the full 8-byte word; extract high byte. The monitor
        // is armed on the granule regardless of access width, so a
        // narrower LDXRB at offset 7 would be equivalent — using the
        // wider load keeps the value flow obvious to the optimiser
        // (and to readers).
        uint64_t v = aarch64::ldxr_load(
            reinterpret_cast<volatile uint64_t *>(&word->val));
        state = static_cast<uint8_t>(v >> 56);
      }
      if (state != expected_state) {
        aarch64::clrex();
        return true;
      }
      if (backend == SpinBackend::WFET)
        aarch64::wfet(deadline);
      else
        aarch64::wfe();
      if (aarch64::read_cntvct() >= deadline)
        return false;
    }
  }
#endif

  // No PAUSE fallback: without a hardware address monitor, PAUSE spin
  // consumes SMT execution resources. Under high contention (16+ threads)
  // multiple threads spin simultaneously, starving the lock holder. Only
  // UMWAIT C0.2 / MWAITX are truly low-power enough for this spin phase.
  (void)expected_state;
  return extract_state(word) != expected_state;
}

} // namespace detail

// Byte-sized state atomic — used by init_latch, indexed_pool, and any
// primitive whose state word is a plain `Atomic<uint8_t>`.
LIBC_INLINE bool spin_on_slot_state(cpp::Atomic<uint8_t> *state,
                                    uint8_t expected_state) {
  return detail::spin_on_state_impl(state, expected_state);
}

// 64-bit packed link word — used by WaitSlot. State is the high byte
// of [state:8 | reserved:8 | tag:32 | next:16]. The detail impl
// byte-loads offset 7 directly rather than doing a full 64-bit load +
// shift on each spin iteration.
//
// Templated on the atomic word type so the same function works for
// both `cpp::Atomic<uint64_t>` (legacy callers) and `cpp::Atomic<Word>`
// where Word is a trivially-copyable 8-byte wrapper (e.g.,
// linkage::Link). The detail impl already extracts the state byte
// via raw pointer-offset addressing, so the wrapper type is
// transparent. spin_wait.h does not include wait_slot.h — the
// template lets Link-typed callers participate without requiring
// the inclusion.
template <typename Word>
LIBC_INLINE bool spin_on_link_state(cpp::Atomic<Word> *link,
                                    uint8_t expected_state) {
  static_assert(sizeof(Word) == 8,
                "spin_on_link_state: expects 8-byte atomic word");
  return detail::spin_on_state_impl(link, expected_state);
}

// Spin on a 64-bit link word until a specific bit is observed CLEAR.
// Returns true on success (bit cleared within budget), false on timeout.
//
// Used by harris_unlink's mark-conflict path: walker observes
// LINK_MARK_BIT set on prev/curr (another walker is mid-splice),
// HW-monitors the slot's cache line until the marker's
// link_finalize_after_splice or link_clear_mark CAS lands. UMWAIT C0.2
// puts us in deep sleep until the line is written, so the wait is
// near-zero power and instantly responsive — typical mark lifetime is
// ~50-100ns (mark CAS → parent CAS → finalize CAS), well under the
// spin budget.
//
// Unrelated writers to slot.link (waker pre-mark, owner state CAS,
// reclaim) ALL preserve MARK via LINK_PRESERVE_MASK, so the bit
// remains set until the marker themselves clears it. This makes
// "bit cleared" a precise signal for "marker finished its operation"
// — the cache-line wake from any other writer just causes one extra
// monitor re-arm + load + sleep cycle.
//
// `mask` is a single-bit mask (caller passes linkage::LINK_MARK_BIT).
// Same UMWAIT/MWAITX skeleton as spin_on_link_state; on hardware
// without an address monitor, returns the current bit-clear state
// once and exits (no PAUSE polling — same SMT-fairness rationale).
//
// Budget: threshold × 256 TSC ticks (~900ns at 4.5GHz with the typical
// kuser_spin_threshold=16). Tuned to cover a contended marker's full
// CAS sequence with headroom; preempted markers fall through to
// timeout, where the caller yields.
// Templated on Word so callers can pass either `cpp::Atomic<uint64_t>*`
// or `cpp::Atomic<linkage::Link>*` (or any other 8-byte trivially-
// copyable atomic wrapper) — the bit test reads the underlying 64-bit
// memory regardless of the typed wrapper. Internally `__builtin_bit_
// cast` converts each load result to a uint64_t for the bit test;
// it folds to a no-op for both the bare-uint64_t path and any layout-
// compatible wrapper, with no runtime cost.
template <typename Word>
LIBC_INLINE bool spin_on_bit_clear(cpp::Atomic<Word> *link, uint64_t mask) {
  static_assert(sizeof(Word) == 8,
                "spin_on_bit_clear: expects 8-byte atomic word");
  // Local helper — extract the underlying 64 bits as an integer so
  // the `& mask` test compiles for both Word=uint64_t and Word being
  // a typed wrapper. __builtin_bit_cast requires both types to be
  // trivially copyable; cpp::Atomic already enforces that on Word
  // via its own static_assert.
  auto raw_bits = [link]() -> uint64_t {
    return __builtin_bit_cast(uint64_t,
                               link->load(cpp::MemoryOrder::RELAXED));
  };

  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return (raw_bits() & mask) == 0;

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &link->val;

  if (backend == SpinBackend::UMWAIT) {
    // Early-exit: mark already cleared (marker finished between
    // walker's bail decision and this call).
    if ((raw_bits() & mask) == 0)
      return true;

    uint64_t deadline = __builtin_ia32_rdtsc() +
                        (static_cast<uint64_t>(threshold) << 8);
    uint32_t lo = static_cast<uint32_t>(deadline);
    uint32_t hi = static_cast<uint32_t>(deadline >> 32);

    for (;;) {
      // Arm monitor, then re-load to close the race between the
      // pre-arm load and UMONITOR (canonical ordering).
      __asm__ volatile("umonitor %0"
                       : : "r"(addr), "m"(link->val) :);
      if ((raw_bits() & mask) == 0)
        return true;
      uint8_t timed_out;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(timed_out)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
      if (timed_out)
        return false;
    }
  }

  if (backend == SpinBackend::MWAITX) {
    const uint32_t budget = static_cast<uint32_t>(threshold) << 8;
    const uint64_t deadline = __builtin_ia32_rdtsc() + budget;

    __asm__ volatile("monitorx"
                     : : "a"(addr), "c"(0), "d"(0), "m"(link->val) :);
    if ((raw_bits() & mask) == 0)
      return true;
    __asm__ volatile("mwaitx"
                     : : "a"(0), "c"(2), "b"(budget)
                     : "memory");

    for (;;) {
      __asm__ volatile("monitorx"
                       : : "a"(addr), "c"(0), "d"(0), "m"(link->val) :);
      if ((raw_bits() & mask) == 0)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint32_t ticks = static_cast<uint32_t>(deadline - now);
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (backend == SpinBackend::WFET || backend == SpinBackend::WFE) {
    uint64_t deadline = aarch64::read_cntvct() +
        (static_cast<uint64_t>(threshold)
         << (8 - aarch64::kCntvctShiftFromTsc > 0
                ? 8 - aarch64::kCntvctShiftFromTsc : 1));

    for (;;) {
      uint64_t v = aarch64::ldxr_load(
          reinterpret_cast<volatile uint64_t *>(&link->val));
      if ((v & mask) == 0) {
        aarch64::clrex();
        return true;
      }
      if (backend == SpinBackend::WFET)
        aarch64::wfet(deadline);
      else
        aarch64::wfe();
      if (aarch64::read_cntvct() >= deadline)
        return false;
    }
  }
#endif

  // No HW monitor — return current bit-clear state without polling.
  // PAUSE-spin would burn SMT execution slots; the walker's caller
  // falls through to NtYieldExecution() on our `false` return, which
  // is the correct hand-off when no monitor is available.
  return (raw_bits() & mask) == 0;
}

// Spin on a 64-bit link word until a specific bit is observed SET.
// Returns true on success (bit set within budget), false on timeout.
//
// Inverse of spin_on_bit_clear. Used by self_splice_if_orphan to wait
// for an external splicer's link_finalize_after_splice CAS (which
// publishes LINK_CERT_BIT) to land. The wait window is the gap between
// the splicer's parent.link CAS (target now off-chain) and their
// finalize CAS on target.link (CERT publish) — typically ~one CAS in
// length, but can stretch into the µs range if the splicer is preempted.
//
// Same UMWAIT/MWAITX skeleton as spin_on_bit_clear; flipped exit
// condition. Once any one of the four CERT-publishing actors wins
// (link_finalize_after_splice, link_cas_state_certify,
// link_exchange_state_to_idle_certify, link_cas_state_detached), the
// bit is monotonically set for the rest of this wait cycle, so this
// is a one-shot wait — caller need not re-arm after observing set.
template <typename Word>
LIBC_INLINE bool spin_on_bit_set(cpp::Atomic<Word> *link, uint64_t mask) {
  static_assert(sizeof(Word) == 8,
                "spin_on_bit_set: expects 8-byte atomic word");
  auto raw_bits = [link]() -> uint64_t {
    return __builtin_bit_cast(uint64_t,
                               link->load(cpp::MemoryOrder::RELAXED));
  };

  uint16_t threshold = kuser_spin_threshold();
  if (threshold < 2)
    return (raw_bits() & mask) != 0;

  SpinBackend backend = get_backend();

#if defined(__x86_64__) || defined(_M_X64)
  void *addr = &link->val;

  if (backend == SpinBackend::UMWAIT) {
    // Early-exit: bit already set (publisher landed between caller's
    // pre-spin check and this call).
    if ((raw_bits() & mask) != 0)
      return true;

    uint64_t deadline = __builtin_ia32_rdtsc() +
                        (static_cast<uint64_t>(threshold) << 8);
    uint32_t lo = static_cast<uint32_t>(deadline);
    uint32_t hi = static_cast<uint32_t>(deadline >> 32);

    for (;;) {
      // Arm monitor, then re-load to close the race between the
      // pre-arm load and UMONITOR (canonical ordering).
      __asm__ volatile("umonitor %0"
                       : : "r"(addr), "m"(link->val) :);
      if ((raw_bits() & mask) != 0)
        return true;
      uint8_t timed_out;
      __asm__ volatile("umwait %[ctl]"
                       : "=@ccc"(timed_out)
                       : [ctl] "r"(1u), "d"(hi), "a"(lo)
                       : "memory");
      if (timed_out)
        return false;
    }
  }

  if (backend == SpinBackend::MWAITX) {
    const uint32_t budget = static_cast<uint32_t>(threshold) << 8;
    const uint64_t deadline = __builtin_ia32_rdtsc() + budget;

    __asm__ volatile("monitorx"
                     : : "a"(addr), "c"(0), "d"(0), "m"(link->val) :);
    if ((raw_bits() & mask) != 0)
      return true;
    __asm__ volatile("mwaitx"
                     : : "a"(0), "c"(2), "b"(budget)
                     : "memory");

    for (;;) {
      __asm__ volatile("monitorx"
                       : : "a"(addr), "c"(0), "d"(0), "m"(link->val) :);
      if ((raw_bits() & mask) != 0)
        return true;
      uint64_t now = __builtin_ia32_rdtsc();
      if (now >= deadline)
        return false;
      uint32_t ticks = static_cast<uint32_t>(deadline - now);
      __asm__ volatile("mwaitx"
                       : : "a"(0), "c"(2), "b"(ticks)
                       : "memory");
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (backend == SpinBackend::WFET || backend == SpinBackend::WFE) {
    uint64_t deadline = aarch64::read_cntvct() +
        (static_cast<uint64_t>(threshold)
         << (8 - aarch64::kCntvctShiftFromTsc > 0
                ? 8 - aarch64::kCntvctShiftFromTsc : 1));

    for (;;) {
      uint64_t v = aarch64::ldxr_load(
          reinterpret_cast<volatile uint64_t *>(&link->val));
      if ((v & mask) != 0) {
        aarch64::clrex();
        return true;
      }
      if (backend == SpinBackend::WFET)
        aarch64::wfet(deadline);
      else
        aarch64::wfe();
      if (aarch64::read_cntvct() >= deadline)
        return false;
    }
  }
#endif

  // PAUSE/YIELD fallback (AArch64 always; old x86 without WAITPKG/
  // MWAITX). Cap matches spin_on_raw — deep enough to cover a healthy
  // single-CAS publisher, shallow enough that a preempted publisher
  // hands off to the caller's NtYieldExecution promptly. Unlike
  // spin_on_bit_clear, the publisher is a different thread on a
  // different core, so PAUSE-spin doesn't starve them via SMT
  // contention — the cost-benefit favors a brief burst over an
  // immediate yield.
  uint16_t count = threshold < 64 ? threshold : 64;
  for (uint16_t i = 0; i < count; ++i) {
    if ((raw_bits() & mask) != 0)
      return true;
    relax_processor();
  }
  return (raw_bits() & mask) != 0;
}

} // namespace spin_wait
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SPIN_WAIT_H
