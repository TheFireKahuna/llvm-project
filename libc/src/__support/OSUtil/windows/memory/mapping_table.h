//===-- Lock-free radix-tree mapping registry -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tracks section and file handles for all mmap-created mappings. Required for:
//   - Split-remap (partial munmap, mremap): needs the section handle
//   - msync(MS_SYNC): needs the file handle for FlushFileBuffers durability
//   - NUMA rebind (mbind): needs the section handle for unmap/remap
//
// Three-level radix tree keyed on (view_base >> 16):
//   - L1: runtime-sized root (bits [N:20]) — sized from MaximumUserModeAddress
//   - L2: 1024 entries (bits [19:10]) — page_alloc on demand, 8KB
//   - L3: 1024 slots   (bits [9:0])  — page_alloc on demand, 64KB
//
// Each slot stores one mapping's metadata. The slot position is uniquely
// determined by the address — no hashing, no probing, no tombstones.
// Insert, lookup, remove, and extract are all O(1) worst case.
//
// State machine per slot (encoded in key atomic):
//   0                = empty (never used or cleared)
//   view_base        = live (fields stable, readers safe)
//   view_base | 0x1  = writing (fields being modified, readers retry)
//   view_base | 0x2  = remapping (VA mutation in progress, VEH stalls)
//   view_base | 0x3  = writing+remapping (commit in progress, VEH stalls)
//
// view_base is always >= 64KB aligned, so bits [1:0] are always 0 for
// valid addresses — the low 2 bits are available as state flags.
//
// VEH remap guard: O(high_water_mark_) via demand-committed guard array.
// Each guard stores a Slot* so the VEH handler can access the slot
// without navigating the radix tree on the fault path.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MAPPING_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MAPPING_TABLE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/commit_region.h"
#include "src/__support/OSUtil/windows/memory/view_spec.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// Per-mapping flags stored alongside the section/file handles.
inline constexpr DWORD VM_FLAG_SEC_RESERVE = 0x1;  // SEC_RESERVE demand-commit
inline constexpr DWORD VM_FLAG_PROT_CHANGED = 0x2; // mprotect called on view
// File MAP_PRIVATE via private memory + demand-read. section_handle is NULL;
// file_handle + section_offset encode the backing file and offset.
// VEH demand-commits pages and reads file content via NtReadFile.
inline constexpr DWORD VM_FLAG_FILE_PRIVATE = 0x8;
/// NUMA interleave: VEH demand-commits each page with a rotating NUMA node
/// derived from the numa_interleave_mask stored in the slot's cold union.
inline constexpr DWORD VM_FLAG_NUMA_INTERLEAVE = 0x4;

struct MappingEntry {
  void *view_base;
  SIZE_T view_size;
  ViewSpec spec; // section, file, offset, prot, flags
};

/// Immutable point-in-time snapshot of a radix tree slot.
///
/// Returned by MappingTable::snapshot(). All fields are copied under a
/// versioned double-read protocol — if a concurrent mutation occurs
/// during the copy, the snapshot is discarded and retried. Callers
/// work exclusively with the snapshot, never with live slot fields.
///
/// This eliminates three classes of concurrency bugs:
///   1. ABA: version counter is monotonically increasing, so even if
///      the same view_base is unmapped and remapped, the version differs.
///   2. Partial reads: all fields are copied in one atomic snapshot.
///   3. Use-after-free: snapshot holds copies, not pointers to live data.
struct SlotSnapshot {
  void *view_base;
  SIZE_T view_size;
  HANDLE section_handle;
  HANDLE file_handle;
  LARGE_INTEGER section_offset;
  DWORD view_prot;
  DWORD flags;

  /// Convert to a MappingEntry for callers that need the legacy format.
  MappingEntry to_entry() const {
    return {view_base, view_size,
            {section_handle, file_handle, section_offset, view_prot, flags}};
  }
};

// Now that MappingEntry is complete, define ViewSpec::from_entry.
LIBC_INLINE ViewSpec ViewSpec::from_entry(const MappingEntry &e) {
  return e.spec;
}

/// Lock-free radix-tree registry for mapping metadata.
///
/// Also serves as the remap guard: during VA mutations (partial munmap,
/// mremap, mbind, MAP_FIXED), entries transition to REMAPPING state.
/// The VEH handler stalls faulting threads on REMAPPING entries instead
/// of delivering SIGSEGV, with dead-owner recovery via bounded timeout.
class MappingTable {
  // ---- Key encoding ----
  // Bits [63:16] = view_base >> 16 (nonzero for any valid 64KB-aligned addr)
  // Bits [1:0]   = state flags
  static constexpr uintptr_t KEY_FREE = 0;
  static constexpr uintptr_t WRITING_BIT = 0x1;
  static constexpr uintptr_t REMAPPING_BIT = 0x2;
  static constexpr uintptr_t STATE_MASK = 0x3;

  struct alignas(64) Slot {
    // --- Hot: touched on every access (offset 0, same cache line as key) ---
    cpp::Atomic<uintptr_t> key{0};       // 0
    HANDLE file_handle;                   // 8
    HANDLE section_handle;                // 16
    LARGE_INTEGER section_offset;         // 24
    DWORD view_prot;                      // 32
    cpp::Atomic<DWORD> flags{0};          // 36
    // --- Dual-purpose field (same cache line, tail) ---
    // LIVE: view_size — used by VEH demand-commit for cluster clamping.
    // REMAPPING: guarded range — used by VEH to stall faulting threads.
    SIZE_T extent;                        // 40
    // --- Cold: dual-purpose by state ---
    // Packed to 4-byte alignment so the union is exactly 12 bytes
    // (offset 48–60), leaving room for `version` at offset 60.
    // The union starts at offset 48 (8-byte aligned in practice),
    // so HANDLE/DWORD64 members are naturally aligned despite the pack.
#pragma pack(push, 4)
    union {
      struct { // REMAPPING state: remap guard ownership.
        HANDLE remap_owner_thread;        // 48
        int remap_guard_index;            // 56
      };
      struct { // LIVE + VM_FLAG_NUMA_INTERLEAVE: per-page NUMA rotation.
        DWORD64 numa_interleave_mask;     // 48 — up to 64 nodes
        ULONG numa_node_count;            // 56 — popcount of mask
      };
      struct { // WRITING state: owner tracking for dead-owner recovery.
        DWORD writing_owner_tid;          // 48 — TID from TEB (~1ns)
        uint32_t writing_start_time;      // 52 — low 32 bits of SystemTime
        int writing_pad_;                 // 56 — unused
      };
    };
#pragma pack(pop)
    // Version counter for ABA-safe seqlock reads. Incremented on every
    // WRITING→LIVE or WRITING→FREE transition. Readers compare version
    // before and after reading non-atomic fields — if it changed, a
    // concurrent mutation occurred and the snapshot is discarded.
    // Fits in the 4-byte padding at offset 60 (cold union is 12 bytes).
    cpp::Atomic<uint32_t> version{0};     // 60
  };
  static_assert(sizeof(Slot) == 64, "Slot must be exactly one cache line");

  // ---- Radix tree ----
  //
  // Key = view_base >> 16. The low 20 bits are split into L2/L3 indexes;
  // the remaining high bits index the runtime-sized L1 root.
  static constexpr int L2_BITS = 10;
  static constexpr int L3_BITS = 10;
  static constexpr int L2_SIZE = 1 << L2_BITS;  // 1024
  static constexpr int L3_SIZE = 1 << L3_BITS;  // 1024
  static constexpr int L2_MASK = L2_SIZE - 1;
  static constexpr int L3_MASK = L3_SIZE - 1;

  // L3: 1024 slots × 64 bytes = 64KB, plus a 128-byte occupancy bitmap
  // for bitset-driven iteration (16 × uint64_t = 1024 bits, one per slot).
  // Set on LIVE transitions, cleared on KEY_FREE transitions. Atomic
  // because concurrent register/remove can target different slots in
  // the same L3Page. Total: 65664 bytes → page_alloc rounds to 17 pages.
  struct L3Page {
    Slot slots[L3_SIZE];
    cpp::Atomic<uint64_t> occupancy[16]; // 1024 bits = 1 per slot
  };
  static_assert(sizeof(L3Page) == 65536 + 128);

  // L2: 1024 pointers × 8 bytes = 8KB. Demand-zero from page_alloc.
  struct L2Page {
    cpp::Atomic<L3Page *> children[L2_SIZE];
  };
  static_assert(sizeof(L2Page) == L2_SIZE * sizeof(void *));

  // Wait timeout for concurrent slot writers (1ms).
  static constexpr LONGLONG SLOT_WAIT_100NS = -10000LL;

  // Dead-owner recovery timeout: 50ms in 100ns units (negative = relative).
  static constexpr LONGLONG REMAP_WAIT_TIMEOUT_100NS = -500000LL;

  // Lifecycle states for the lazily initialized mapping-table backing store.
  static constexpr FutexValueType INIT_UNINITIALIZED = 0;
  static constexpr FutexValueType INIT_IN_PROGRESS = 1;
  static constexpr FutexValueType INIT_READY = 2;
  static constexpr FutexValueType INIT_DESTROYED = 3;

  // ---- Data members ----

  // L1 radix root. Sized once from the runtime maximum user VA and then
  // fixed for process lifetime. Zero-filled by page_alloc.
  cpp::Atomic<L2Page *> *l1_ = nullptr;
  size_t l1_size_ = 0;
  uintptr_t max_view_base_ = 0;

  // Guard array lazy init.
  Futex init_state_{INIT_UNINITIALIZED};

  // VEH hot-read path (own cache line to avoid false sharing).
  alignas(64) cpp::Atomic<int> active_remap_count_{0};

  // Diagnostic: number of WRITING slots recovered from dead owners.
  cpp::Atomic<uint32_t> forced_write_recoveries_{0};

  // Remap guard array (demand-committed). Each entry stores a Slot*
  // (as uintptr_t). 0 = free. Nonzero = pointer to a REMAPPING slot.
  static uint32_t guards_per_page() {
    return static_cast<uint32_t>(get_cached_page_size() /
                                 sizeof(cpp::Atomic<uintptr_t>));
  }
  internal::CommitRegion guard_region_;
  cpp::Atomic<uintptr_t> *remap_guards_ = nullptr;
  cpp::Atomic<uint32_t> guard_high_water_{0};
  cpp::Atomic<uint32_t> alloc_cursor_{0};
  static constexpr SIZE_T GUARD_RESERVE_BYTES = 64 * 1024; // 64KB
  static constexpr uint32_t MAX_GUARDS =
      GUARD_RESERVE_BYTES / sizeof(cpp::Atomic<uintptr_t>); // 8192

  // ---- Radix key decomposition ----

  LIBC_INLINE static uintptr_t radix_key(uintptr_t view_base) {
    return view_base >> 16;
  }
  LIBC_INLINE static size_t l1_index(uintptr_t key) {
    return static_cast<size_t>(key >> (L2_BITS + L3_BITS));
  }
  LIBC_INLINE static int l2_index(uintptr_t key) {
    return static_cast<int>((key >> L3_BITS) & L2_MASK);
  }
  LIBC_INLINE static int l3_index(uintptr_t key) {
    return static_cast<int>(key & L3_MASK);
  }

  static uintptr_t runtime_max_view_base() {
    uintptr_t max_address = reinterpret_cast<uintptr_t>(get_max_address());
    if (LIBC_UNLIKELY(max_address < get_alloc_granularity()))
      __builtin_trap();
    return align_down_to_granularity(max_address);
  }

  static size_t required_l1_size(uintptr_t max_view_base) {
    return l1_index(radix_key(max_view_base)) + 1;
  }

  static size_t l1_bytes_for(size_t l1_size) {
    if (LIBC_UNLIKELY(l1_size == 0 ||
                      l1_size > SIZE_MAX / sizeof(cpp::Atomic<L2Page *>)))
      __builtin_trap();
    return l1_size * sizeof(cpp::Atomic<L2Page *>);
  }

  // Reset the root/guard backing state. Before INIT_READY, these are the only
  // auxiliary allocations that can exist; no L2/L3 pages are reachable yet.
  void reset_root_state() {
    if (l1_) {
      internal::page_free(l1_);
      l1_ = nullptr;
    }
    l1_size_ = 0;
    max_view_base_ = 0;
    remap_guards_ = nullptr;
    guard_region_.destroy();
    active_remap_count_.store(0, cpp::MemoryOrder::RELAXED);
    forced_write_recoveries_.store(0, cpp::MemoryOrder::RELAXED);
    guard_high_water_.store(0, cpp::MemoryOrder::RELAXED);
    alloc_cursor_.store(0, cpp::MemoryOrder::RELAXED);
  }

  // ---- Radix navigation ----

  // Find the slot for an address. Returns nullptr if the radix path
  // doesn't exist (no L2/L3 page allocated for this range).
  Slot *find_slot(uintptr_t view_base) {
    if (LIBC_UNLIKELY(!ensure_init() || view_base > max_view_base_))
      return nullptr;
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = l1_[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return nullptr;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return nullptr;
    return &l3->slots[l3_index(k)];
  }

  // page_alloc with retry. 8-64KB allocations fail only under terminal
  // OOM — trapping is better than silent data corruption (live-but-
  // untracked mappings that break munmap/mprotect/msync permanently).
  static void *page_alloc_or_trap(size_t size) {
    void *p = internal::page_alloc(size);
    if (LIBC_LIKELY(p != nullptr))
      return p;
    ::NtYieldExecution();
    p = internal::page_alloc(size);
    if (LIBC_LIKELY(p != nullptr))
      return p;
    __builtin_trap();
  }

  // Find or create the slot for an address. Allocates L2/L3 pages on
  // demand. Never returns nullptr — traps on allocation failure (see
  // page_alloc_or_trap). This makes register_mapping_take infallible,
  // eliminating the class of live-but-untracked mapping bugs.
  // Lock-free: concurrent callers for the same range converge via CAS.
  Slot *ensure_slot(uintptr_t view_base) {
    if (LIBC_UNLIKELY(!ensure_init() || view_base > max_view_base_))
      __builtin_trap(); // Out-of-bounds VA — should never reach here.
    uintptr_t k = radix_key(view_base);
    size_t i1 = l1_index(k);
    int i2 = l2_index(k), i3 = l3_index(k);

    L2Page *l2 = l1_[i1].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2)) {
      auto *fresh =
          static_cast<L2Page *>(page_alloc_or_trap(sizeof(L2Page)));
      L2Page *expected = nullptr;
      if (!l1_[i1].compare_exchange_strong(expected, fresh,
                                           cpp::MemoryOrder::RELEASE,
                                           cpp::MemoryOrder::ACQUIRE)) {
        internal::page_free(fresh);
        l2 = expected; // another thread won
      } else {
        l2 = fresh;
      }
    }

    L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3)) {
      auto *fresh =
          static_cast<L3Page *>(page_alloc_or_trap(sizeof(L3Page)));
      L3Page *expected = nullptr;
      if (!l2->children[i2].compare_exchange_strong(
              expected, fresh, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::ACQUIRE)) {
        internal::page_free(fresh);
        l3 = expected;
      } else {
        l3 = fresh;
      }
    }

    return &l3->slots[i3];
  }

  // ---- Guard array init ----

  bool ensure_init() {
    FutexValueType state = init_state_.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(state == INIT_READY))
      return true;
    if (LIBC_UNLIKELY(state == INIT_DESTROYED))
      return false;
    if (state == INIT_UNINITIALIZED &&
        init_state_.compare_exchange_strong(state, INIT_IN_PROGRESS,
                                            cpp::MemoryOrder::ACQUIRE)) {
      uintptr_t max_view_base = runtime_max_view_base();
      size_t l1_size = required_l1_size(max_view_base);

      // Publish acquired root state immediately so the fork child can
      // reclaim any partially initialized backing store it inherits.
      l1_ = static_cast<cpp::Atomic<L2Page *> *>(
          page_alloc_or_trap(l1_bytes_for(l1_size)));
      l1_size_ = l1_size;
      max_view_base_ = max_view_base;

      if (LIBC_UNLIKELY(!guard_region_.init(GUARD_RESERVE_BYTES)))
        __builtin_trap();
      // Commit the entire 64KB guard region upfront so expand_guards()
      // never races VEH on uncommitted memory. 64KB (8192 slots) is
      // negligible and eliminates the need for per-slot demand-commit.
      if (LIBC_UNLIKELY(!guard_region_.ensure_committed(GUARD_RESERVE_BYTES)))
        __builtin_trap();
      remap_guards_ = guard_region_.as<cpp::Atomic<uintptr_t>>();

      // Pin L1 root so the VEH hot path never faults on radix lookup (RA14.8).
      pin_critical_pages();

      init_state_.store_and_notify_all(INIT_READY);
      return true;
    }
    while ((state = init_state_.load(cpp::MemoryOrder::ACQUIRE)) ==
           INIT_IN_PROGRESS)
      init_state_.wait(INIT_IN_PROGRESS);
    return state == INIT_READY;
  }

  /// Pin the L1 radix root into physical memory via NtLockVirtualMemory.
  /// Prevents page-out of the hot VEH lookup path (~256KB, within default
  /// 128KB working-set quota). Best-effort: failure is non-fatal.
  void pin_critical_pages() {
    if (!l1_ || l1_size_ == 0)
      return;
    PVOID base = l1_;
    SIZE_T size = l1_bytes_for(l1_size_);
    ::NtLockVirtualMemory(NtCurrentProcess(), &base, &size, MAP_PROCESS);
  }

  // ---- Thread liveness ----

  static bool is_thread_dead(HANDLE h) {
    if (!h)
      return true;
    LARGE_INTEGER zero_timeout = {};
    NTSTATUS st = ::NtWaitForSingleObject(h, 0, &zero_timeout);
    return st == /*STATUS_WAIT_0*/ 0;
  }

  static HANDLE open_current_thread_handle() {
    HANDLE h = nullptr;
    ::NtDuplicateObject(NtCurrentProcess(), NtCurrentThread(),
                        NtCurrentProcess(), &h,
                        SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, 0, 0);
    return h;
  }

  // ---- Handle helpers ----

  static HANDLE dup_handle(HANDLE h) {
    if (!h)
      return nullptr;
    HANDLE dup = nullptr;
    NTSTATUS st = ::NtDuplicateObject(NtCurrentProcess(), h,
                                      NtCurrentProcess(), &dup,
                                      0, 0, DUPLICATE_SAME_ACCESS);
    return NT_SUCCESS(st) ? dup : nullptr;
  }

  // ---- Slot helpers ----

  // Publish a new key value (LIVE or KEY_FREE) and increment the version
  // counter. Called at every WRITING→LIVE and WRITING→FREE transition.
  // The version bump is the RELEASE store that makes all prior field
  // writes visible; the key store that follows wakes waiters.
  static void publish_slot(Slot *s, uintptr_t new_key) {
    s->version.fetch_add(1, cpp::MemoryOrder::RELEASE);
    s->key.store(new_key, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&s->key, UINT32_MAX);
  }

  // ---- L3Page occupancy bitmap ----

  // Core bitmap update given a pre-resolved L3Page and slot index.
  // Avoids redundant radix re-navigation when the caller already has
  // the L3Page (e.g., from find_slot/ensure_slot).
  LIBC_INLINE static void set_occupancy(L3Page *l3, unsigned i3) {
    l3->occupancy[i3 / 64].fetch_or(1ULL << (i3 % 64),
                                     cpp::MemoryOrder::RELEASE);
  }
  LIBC_INLINE static void clear_occupancy(L3Page *l3, unsigned i3) {
    l3->occupancy[i3 / 64].fetch_and(~(1ULL << (i3 % 64)),
                                      cpp::MemoryOrder::RELEASE);
  }

  // Set a slot's occupancy bit via address (radix lookup). Idempotent.
  // Used on mutation paths where the L3Page isn't readily available.
  LIBC_INLINE void set_occupancy_bit(uintptr_t view_base) {
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = l1_[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return;
    set_occupancy(l3, static_cast<unsigned>(l3_index(k)));
  }

  // Clear a slot's occupancy bit via address (radix lookup). Idempotent.
  LIBC_INLINE void clear_occupancy_bit(uintptr_t view_base) {
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = l1_[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return;
    clear_occupancy(l3, static_cast<unsigned>(l3_index(k)));
  }

  /// Write all slot fields, replacing flags unconditionally.
  /// Used for fresh registrations where no prior flag state exists.
  static void write_slot_replace_flags(Slot *s, const ViewSpec &spec,
                                       SIZE_T size) {
    s->file_handle = spec.file;
    s->section_handle = spec.section;
    s->section_offset = spec.offset;
    s->extent = size;
    s->view_prot = spec.prot;
    s->flags.store(spec.flags, cpp::MemoryOrder::RELAXED);
  }

  /// Write all slot fields, OR-ing new flags into existing flags.
  /// Used after REMAPPING to preserve flags (e.g. NUMA interleave)
  /// that were set while the slot was in REMAPPING state.
  static void write_slot_accumulate_flags(Slot *s, const ViewSpec &spec,
                                          SIZE_T size) {
    s->file_handle = spec.file;
    s->section_handle = spec.section;
    s->section_offset = spec.offset;
    s->extent = size;
    s->view_prot = spec.prot;
    if (spec.flags)
      s->flags.fetch_or(spec.flags, cpp::MemoryOrder::RELAXED);
  }

  static void clear_handles(Slot *s) {
    s->file_handle = nullptr;
    s->section_handle = nullptr;
  }

  static void close_owner_handle(Slot *s) {
    if (s->remap_owner_thread) {
      ::NtClose(s->remap_owner_thread);
      s->remap_owner_thread = nullptr;
    }
  }

  static void close_slot_handles(Slot *s) {
    if (s->file_handle)
      ::NtClose(s->file_handle);
    if (s->section_handle)
      ::NtClose(s->section_handle);
    clear_handles(s);
  }

  // ---- WRITING state owner tracking ----

  // Low 32 bits of system time (100ns units since 1601-01-01). Direct
  // KUSER_SHARED_DATA read — zero syscalls. Same epoch as
  // KERNEL_USER_TIMES.CreateTime, enabling TID-reuse detection.
  // Wraps every ~429 seconds, well beyond the 100ms liveness threshold.
  static uint32_t system_time_lo() {
    return static_cast<uint32_t>(
        *reinterpret_cast<const volatile ULONGLONG *>(0x7FFE0014ULL));
  }

  // 100ms in system time units (100ns each).
  static constexpr uint32_t WRITING_LIVENESS_THRESHOLD = 1000000; // 100ms

  // Stamp a slot entering WRITING state with owner TID and timestamp.
  static void stamp_writing_owner(Slot *s) {
    s->writing_owner_tid = NtCurrentThreadId();
    s->writing_start_time = system_time_lo();
  }

  // Check if a WRITING slot's owner thread is dead or its TID was reused.
  // Only called after the WRITING_LIVENESS_THRESHOLD has elapsed.
  //
  // Three-stage check:
  //   1. NtOpenThread fails → TID doesn't exist → dead.
  //   2. Thread handle is signaled (NtWaitForSingleObject) → dead.
  //   3. Thread is alive but was created AFTER the WRITING stamp →
  //      the TID was recycled by a new thread → original owner is dead.
  //
  // Stage 3 prevents permanent livelock from TID reuse: without it, a
  // recycled TID looks alive and recovery never fires. The comparison
  // uses system time (same epoch as KERNEL_USER_TIMES.CreateTime).
  static bool is_writing_owner_dead(Slot *s) {
    DWORD tid = s->writing_owner_tid;
    if (tid == 0)
      return true;
    CLIENT_ID cid = {};
    cid.UniqueThread = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(tid));
    auto oa = internal_oa();
    HANDLE h = nullptr;
    NTSTATUS st = ::NtOpenThread(
        &h, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, &oa, &cid);
    if (!NT_SUCCESS(st))
      return true; // Thread doesn't exist — dead.

    // Stage 2: check if thread has exited.
    bool dead = is_thread_dead(h);
    if (dead) {
      ::NtClose(h);
      return true;
    }

    // Stage 3: TID reuse detection. Query the thread's creation time
    // and compare against the WRITING stamp. If the thread was created
    // after the slot was stamped, the TID was recycled — the original
    // owner is dead even though this handle points to a live thread.
    KERNEL_USER_TIMES times;
    st = ::NtQueryInformationThread(h, ThreadTimes, &times,
                                    sizeof(times), nullptr);
    ::NtClose(h);

    if (NT_SUCCESS(st)) {
      // Both are system time (100ns since 1601-01-01). Compare low 32
      // bits via unsigned subtraction — correct for elapsed < ~214s
      // (half the 429s wrap period). We check after only ~100ms of
      // waiting, so any reuse thread has delta well within range.
      uint32_t create_lo = static_cast<uint32_t>(times.CreateTime.QuadPart);
      uint32_t delta = create_lo - s->writing_start_time;
      // If the thread was created after the stamp (delta > 0 and not
      // a wrap-around artifact), the TID was reused.
      if (delta > 0 && delta < 0x80000000u)
        return true;
    }
    // Query failed or thread was created before the stamp → genuine
    // owner, still alive. Conservative: do not force-recover.
    return false;
  }

  // Recover a WRITING slot whose owner is confirmed dead.
  // CASes key to KEY_FREE and closes any partially-written handles.
  bool recover_dead_write(Slot *s, uintptr_t current_key) {
    uintptr_t expected = current_key;
    if (!s->key.compare_exchange_strong(expected, KEY_FREE,
                                        cpp::MemoryOrder::ACQ_REL))
      return false;
    close_slot_handles(s);
    forced_write_recoveries_.fetch_add(1, cpp::MemoryOrder::RELAXED);
    s->version.fetch_add(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&s->key, UINT32_MAX);
    clear_occupancy_bit(current_key & ~STATE_MASK);
    return true;
  }

  // Wait for a WRITING slot with dead-owner recovery.
  //
  // Protocol:
  //   - For the first 100ms: futex_addr::wait_nt only (no liveness probes).
  //   - After 100ms: check is_writing_owner_dead() on every wake.
  //   - If dead: CAS to KEY_FREE, close handles, wake waiters.
  //   - If alive: continue waiting (emit diagnostic after 1s).
  //
  // Recovery fires ONLY when the owner thread is confirmed dead.
  // A live-but-stalled thread (e.g., debugger-suspended) is never
  // force-recovered, preserving the invariant that only the thread
  // that CAS'd to WRITING can transition to LIVE.
  void wait_for_writing(Slot *s, uintptr_t writing_key) {
    uint32_t start_time = system_time_lo();
    bool past_threshold = false;

    for (;;) {
      uintptr_t k = s->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k != writing_key)
        return; // State changed — caller re-evaluates.

      if (!past_threshold) {
        uint32_t elapsed = system_time_lo() - start_time;
        if (elapsed >= WRITING_LIVENESS_THRESHOLD)
          past_threshold = true;
      }

      if (past_threshold && is_writing_owner_dead(s)) {
        recover_dead_write(s, writing_key);
        return;
      }

      wait_for_slot(s, writing_key);
    }
  }

  // ---- Slot wait/recovery helpers ----

  static void wait_for_slot(Slot *s, uintptr_t current_key) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = SLOT_WAIT_100NS;
    futex_addr::wait_nt(
        reinterpret_cast<const volatile uintptr_t *>(&s->key),
        current_key, &timeout);
  }

  // Recover a REMAPPING slot whose owner thread has died.
  // Caller must have verified (key & REMAPPING_BIT) and is_thread_dead().
  // Returns true if this thread won the CAS and performed cleanup.
  bool recover_dead_remap(Slot *s, uintptr_t current_key) {
    uintptr_t expected = current_key;
    if (!s->key.compare_exchange_strong(expected, KEY_FREE,
                                        cpp::MemoryOrder::ACQ_REL))
      return false;
    close_slot_handles(s);
    close_owner_handle(s);
    int gi = s->remap_guard_index;
    release_guard(gi);
    // SEQ_CST: the VEH fast-path loads active_remap_count_ with ACQUIRE.
    // RELEASE here is insufficient on ARM64 — the VEH thread could observe
    // count==0 while the REMAPPING→KEY_FREE key transition is still invisible.
    // SEQ_CST ensures the key store is globally visible before the count drops.
    if (active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1)
      try_shrink_guards();
    s->version.fetch_add(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&s->key, UINT32_MAX);
    clear_occupancy_bit(current_key & ~STATE_MASK);
    return true;
  }

  // If the slot is stuck in REMAPPING with a dead owner, recover it.
  void try_recover_slot(Slot *s, uintptr_t k) {
    if ((k & REMAPPING_BIT) && is_thread_dead(s->remap_owner_thread))
      recover_dead_remap(s, k);
  }

  // ---- Remap guard array operations ----

  uint32_t expand_guards() {
    // Safe to use fetch_add: the entire guard region is pre-committed
    // at init time (ensure_init), so VEH can never fault on uncommitted
    // memory. Freshly committed pages are zero-filled — VEH sees ptr==0
    // and skips the slot until claim_guard() stores the Slot* pointer.
    uint32_t idx = guard_high_water_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    if (LIBC_UNLIKELY(idx >= MAX_GUARDS)) {
      guard_high_water_.fetch_sub(1, cpp::MemoryOrder::RELAXED);
      return MAX_GUARDS;
    }
    return idx;
  }

  void ensure_hwm_covers(uint32_t min_hwm) {
    uint32_t hwm = guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    while (hwm < min_hwm) {
      if (guard_high_water_.compare_exchange_weak(
              hwm, min_hwm, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::ACQUIRE))
        return;
    }
  }

  // Claim a guard slot for a REMAPPING operation. Stores a Slot*
  // (as uintptr_t) so the VEH handler can access the slot directly
  // without navigating the radix tree on the fault path.
  int claim_guard(Slot *slot) {
    if (LIBC_UNLIKELY(!remap_guards_))
      return -1;
    uintptr_t encoded = reinterpret_cast<uintptr_t>(slot);

    for (;;) {
      uint32_t hwm = guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);

      if (hwm > 0) {
        uint32_t start =
            alloc_cursor_.fetch_add(1, cpp::MemoryOrder::RELAXED) % hwm;
        for (uint32_t i = 0; i < hwm; ++i) {
          uint32_t gi = (start + i) % hwm;
          uintptr_t expected = 0;
          if (remap_guards_[gi].compare_exchange_strong(
                  expected, encoded, cpp::MemoryOrder::RELEASE,
                  cpp::MemoryOrder::RELAXED)) {
            ensure_hwm_covers(gi + 1);
            return static_cast<int>(gi);
          }
        }
      }

      uint32_t gi = expand_guards();
      if (LIBC_LIKELY(gi < MAX_GUARDS)) {
        remap_guards_[gi].store(encoded, cpp::MemoryOrder::RELEASE);
        ensure_hwm_covers(gi + 1);
        return static_cast<int>(gi);
      }

      // Guard array full. Sweep for dead owners before retrying.
      uint32_t recycled = sweep_dead_guards();
      if (recycled > 0)
        continue; // Freed slots available — retry scan.

      // All MAX_GUARDS guards are held by live threads. Return -1 so the
      // caller proceeds without a VEH guard entry. The REMAPPING key state
      // still protects the VA range — the VEH handler will detect it via
      // the radix tree lookup (slower O(1) path instead of the O(guard_hwm)
      // scan, but correct). This avoids unbounded spinning when many
      // concurrent remaps saturate the guard array.
      return -1;
  } // for(;;)
  } // claim_guard

  // Sweep the guard array for stale entries. Called when claim_guard()
  // hits capacity. Returns count of freed guards.
  //
  // Two types of stale entry:
  //   1. Guard points to a slot no longer in REMAPPING (remap completed
  //      but guard wasn't released — shouldn't happen, but defense-in-depth).
  //   2. Slot is REMAPPING but owner thread is dead (killed mid-remap).
  //      Full dead-remap recovery is performed, which frees the guard.
  //
  // O(high_water_mark), same as the VEH guard scan.
  uint32_t sweep_dead_guards() {
    uint32_t hwm = guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    uint32_t recycled = 0;

    for (uint32_t i = 0; i < hwm; ++i) {
      uintptr_t ptr = remap_guards_[i].load(cpp::MemoryOrder::ACQUIRE);
      if (!ptr)
        continue;

      Slot *slot = reinterpret_cast<Slot *>(ptr);
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      // Guard points to slot no longer in REMAPPING — stale.
      if (!(k & REMAPPING_BIT)) {
        uintptr_t expected = ptr;
        if (remap_guards_[i].compare_exchange_strong(
                expected, 0, cpp::MemoryOrder::RELEASE,
                cpp::MemoryOrder::RELAXED))
          ++recycled;
        continue;
      }

      // Slot is REMAPPING but owner is dead — full recovery.
      if (is_thread_dead(slot->remap_owner_thread)) {
        if (recover_dead_remap(slot, k))
          ++recycled;
      }
    }
    return recycled;
  }

  void release_guard(int guard_index) {
    if (LIBC_UNLIKELY(guard_index < 0))
      return;
    remap_guards_[guard_index].store(0, cpp::MemoryOrder::RELEASE);
  }

  // Shrink high_water_mark_ after the last active remap completes.
  void try_shrink_guards() {
    uint32_t hwm = guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    if (hwm == 0)
      return;
    if (active_remap_count_.load(cpp::MemoryOrder::ACQUIRE) != 0)
      return;

    uint32_t new_hwm = 0;
    for (uint32_t i = 0; i < hwm; ++i) {
      if (remap_guards_[i].load(cpp::MemoryOrder::ACQUIRE) != 0)
        new_hwm = i + 1;
    }

    if (new_hwm < hwm) {
      guard_high_water_.compare_exchange_strong(
          hwm, new_hwm, cpp::MemoryOrder::RELEASE,
          cpp::MemoryOrder::RELAXED);
    }
  }

public:
  // -----------------------------------------------------------------------
  // VEH fast-path query
  // -----------------------------------------------------------------------

  /// Number of in-progress remap operations. The memory fault VEH handler
  /// checks this first — zero means no remap guard scan is needed.
  int active_remap_count() {
    return active_remap_count_.load(cpp::MemoryOrder::ACQUIRE);
  }

  /// Diagnostic: number of WRITING slots recovered from dead owners.
  uint32_t get_forced_write_recoveries() {
    return forced_write_recoveries_.load(cpp::MemoryOrder::RELAXED);
  }

  // -----------------------------------------------------------------------
  // Basic operations — O(1) worst case via radix lookup
  // -----------------------------------------------------------------------

  /// Register a mapping. Duplicates non-null handles from spec.
  /// Returns false if handle duplication fails (handle table exhaustion).
  bool register_mapping(void *view_base, SIZE_T view_size, ViewSpec spec) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    HANDLE dup_file = dup_handle(spec.file);
    if (spec.file && !dup_file)
      return false;
    HANDLE dup_section = dup_handle(spec.section);
    if (spec.section && !dup_section) {
      if (dup_file)
        ::NtClose(dup_file);
      return false;
    }

    ViewSpec dup_spec = spec;
    dup_spec.file = dup_file;
    dup_spec.section = dup_section;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE) {
        uintptr_t expected = KEY_FREE;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          stamp_writing_owner(slot);
          write_slot_replace_flags(slot, dup_spec, view_size);
          set_occupancy_bit(target); // Before publish: visible by the
          publish_slot(slot, target); // time any reader sees LIVE key.
          return true;
        }
        continue;
      }

      if (k == target) {
        uintptr_t expected = target;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          stamp_writing_owner(slot);
          close_slot_handles(slot);
          write_slot_replace_flags(slot, dup_spec, view_size);
          // Bit already set (re-register of LIVE slot).
          publish_slot(slot, target);
          return true;
        }
        continue;
      }

      // Writer or remapper active — recover dead owners, then wait.
      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
      } else {
        try_recover_slot(slot, k);
        wait_for_slot(slot, k);
      }
    }
  }

  /// Register a mapping, taking ownership of the handles (no duplication).
  /// Saves ~600ns per mmap (2 NtDuplicateObject + 1 NtClose eliminated).
  bool register_mapping_take(void *view_base, SIZE_T view_size,
                             ViewSpec spec) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE) {
        uintptr_t expected = KEY_FREE;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          stamp_writing_owner(slot);
          write_slot_replace_flags(slot, spec, view_size);
          set_occupancy_bit(target);
          publish_slot(slot, target);
          return true;
        }
        continue;
      }

      if (k == target) {
        uintptr_t expected = target;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          stamp_writing_owner(slot);
          close_slot_handles(slot);
          write_slot_replace_flags(slot, spec, view_size);
          // Bit already set (re-register of LIVE slot).
          publish_slot(slot, target);
          return true;
        }
        continue;
      }

      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
      } else {
        try_recover_slot(slot, k);
        wait_for_slot(slot, k);
      }
    }
  }

  /// Atomically OR additional flags. Lock-free via Atomic<DWORD>.
  /// Post-write key recheck: if the slot was freed between the key
  /// check and the fetch_or, undo the write to prevent stale flags
  /// from leaking to a future reuse of this slot.
  void add_flags(void *view_base, DWORD new_flags) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return;

      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
        continue;
      }

      // LIVE or REMAPPING — flag OR is safe in either state.
      slot->flags.fetch_or(new_flags, cpp::MemoryOrder::RELEASE);

      // Recheck: if the slot was freed or reassigned during the
      // fetch_or window, undo the flags to prevent stale flag leakage
      // to a future mapping at this address.
      uintptr_t k_after = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (LIBC_UNLIKELY(k_after == KEY_FREE ||
                        (k_after & ~STATE_MASK) != target))
        slot->flags.fetch_and(~new_flags, cpp::MemoryOrder::RELAXED);
      return;
    }
  }

  /// Atomically clear specific flags. Lock-free via Atomic<DWORD>.
  /// Same key-recheck pattern as add_flags to prevent operating on a
  /// freed/reassigned slot.
  void remove_flags(void *view_base, DWORD clear_flags) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k == KEY_FREE || (k & WRITING_BIT))
      return;
    if ((k & ~STATE_MASK) != target)
      return;

    slot->flags.fetch_and(~clear_flags, cpp::MemoryOrder::RELEASE);
  }

  /// Set NUMA interleave policy on a LIVE slot. Stores the node mask and
  /// precomputed node count in the cold union (dual-purposed from remap
  /// fields, which are unused in LIVE state). Sets VM_FLAG_NUMA_INTERLEAVE.
  void set_numa_interleave(void *view_base, DWORD64 mask) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE || (k & STATE_MASK))
        return; // Only set on LIVE slots.
      if ((k & ~STATE_MASK) != target)
        return;

      slot->numa_interleave_mask = mask;
      slot->numa_node_count = static_cast<ULONG>(__builtin_popcountll(mask));
      slot->flags.fetch_or(VM_FLAG_NUMA_INTERLEAVE, cpp::MemoryOrder::RELEASE);
      return;
    }
  }

  /// Read the NUMA interleave mask from a LIVE slot. Returns 0 if not set.
  DWORD64 get_numa_interleave_mask(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return 0;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if ((k & ~STATE_MASK) != target)
      return 0;
    if (!(slot->flags.load(cpp::MemoryOrder::RELAXED) & VM_FLAG_NUMA_INTERLEAVE))
      return 0;
    return slot->numa_interleave_mask;
  }

  /// Take a consistent, ABA-safe snapshot of a LIVE slot. O(1).
  ///
  /// Uses a versioned double-read protocol:
  ///   1. Load version (ACQUIRE) and key (ACQUIRE).
  ///   2. Copy all fields using relaxed atomic loads (ARM64-safe).
  ///   3. Acquire fence.
  ///   4. Reload version (RELAXED) and key (RELAXED).
  ///   5. If both match → snapshot is consistent. Otherwise retry.
  ///
  /// The version counter eliminates ABA: even if the same view_base
  /// is unmapped and remapped between steps 1 and 4, the version will
  /// differ because every WRITING→LIVE/FREE transition increments it.
  ///
  /// Non-atomic fields are read via __atomic_load_n(RELAXED) to prevent
  /// the compiler from hoisting them above the key check on ARM64.
  /// This is the single entry point for all read-only queries — replaces
  /// the former lookup(), lookup_entry(), and lookup_commit_info().
  bool snapshot(void *view_base, SlotSnapshot *out) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return false;

    for (;;) {
      uint32_t v1 = slot->version.load(cpp::MemoryOrder::ACQUIRE);
      uintptr_t k1 = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (LIBC_UNLIKELY(k1 != target))
        return false;

      // Read all fields. Non-atomic fields use __atomic_load_n with
      // relaxed ordering to establish a compiler barrier — prevents
      // the compiler from reordering these loads past the second key
      // check on weakly-ordered architectures (ARM64).
      out->view_base = view_base;
      out->view_size = __atomic_load_n(&slot->extent, __ATOMIC_RELAXED);
      out->section_handle =
          __atomic_load_n(&slot->section_handle, __ATOMIC_RELAXED);
      out->file_handle =
          __atomic_load_n(&slot->file_handle, __ATOMIC_RELAXED);
      LARGE_INTEGER off;
      off.QuadPart =
          __atomic_load_n(&slot->section_offset.QuadPart, __ATOMIC_RELAXED);
      out->section_offset = off;
      out->view_prot =
          __atomic_load_n(&slot->view_prot, __ATOMIC_RELAXED);
      out->flags = slot->flags.load(cpp::MemoryOrder::RELAXED);

      // Acquire fence: ensures the field reads above are complete
      // before we check the version/key for consistency.
      cpp::atomic_thread_fence(cpp::MemoryOrder::ACQUIRE);
      uint32_t v2 = slot->version.load(cpp::MemoryOrder::RELAXED);
      uintptr_t k2 = slot->key.load(cpp::MemoryOrder::RELAXED);

      if (LIBC_LIKELY(v1 == v2 && k1 == k2))
        return true;

      // Version or key changed — a concurrent mutation invalidated
      // the snapshot. If the key is no longer our target, the slot
      // was freed or reassigned — return false. Otherwise retry.
      if ((k2 & ~STATE_MASK) != target)
        return false;
    }
  }

  /// Remove and transfer handle ownership to caller. O(1).
  bool extract(void *view_base, MappingEntry *out) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return false;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return false;

      if (k == target) {
        uintptr_t expected = target;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          stamp_writing_owner(slot);
          out->view_base = view_base;
          out->view_size = slot->extent;
          out->spec = {slot->section_handle, slot->file_handle,
                       slot->section_offset, slot->view_prot,
                       slot->flags.load(cpp::MemoryOrder::RELAXED)};
          clear_handles(slot);
          publish_slot(slot, KEY_FREE);
          clear_occupancy_bit(target);
          return true;
        }
        continue;
      }

      // WRITING/REMAPPING — wait for resolution.
      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }
      return false;
    }
  }

  /// Remove and close handles. O(1).
  void remove(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return;

      if (k == target) {
        uintptr_t expected = target;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          stamp_writing_owner(slot);
          close_slot_handles(slot);
          publish_slot(slot, KEY_FREE);
          clear_occupancy_bit(target);
          return;
        }
        continue;
      }

      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }
      return;
    }
  }

  // -----------------------------------------------------------------------
  // Transactional remap API
  // -----------------------------------------------------------------------
  //
  // Protocol:
  //   begin_remap(view_base, size, &entry)  — LIVE -> REMAPPING
  //   ... perform VA mutation (unmap, split, remap) ...
  //   commit_remap(view_base, ...)          — REMAPPING -> LIVE (new data)
  //     or
  //   abort_remap(view_base)                — REMAPPING -> LIVE (restore)
  //
  // For MAP_FIXED on ranges with no prior entry, use begin_remap_guard
  // to create a sentinel REMAPPING entry.

  /// LIVE -> WRITING -> write guard metadata -> REMAPPING.
  /// Snapshots entry into *out. O(1).
  bool begin_remap(void *view_base, SIZE_T guarded_size, MappingEntry *out) {
    if (!ensure_init())
      return false;

    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return false;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return false;

      if (k == target) {
        uintptr_t expected = target;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          out->view_base = view_base;
          out->view_size = slot->extent;
          out->spec = {slot->section_handle, slot->file_handle,
                       slot->section_offset, slot->view_prot,
                       slot->flags.load(cpp::MemoryOrder::RELAXED)};
          slot->extent = guarded_size;
          slot->remap_owner_thread = open_current_thread_handle();
          slot->remap_guard_index = claim_guard(slot);
          active_remap_count_.fetch_add(1, cpp::MemoryOrder::RELEASE);
          slot->key.store(target | REMAPPING_BIT,
                          cpp::MemoryOrder::RELEASE);
          return true;
        }
        continue;
      }

      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }
      return false;
    }
  }

  /// Create a REMAPPING sentinel for MAP_FIXED.
  ///
  /// If the slot is KEY_FREE, creates a fresh sentinel (zeroed ViewSpec).
  /// If the slot is LIVE at the target address, takes it over — existing
  /// handles are left in the slot and will be closed by abort_remap_guard
  /// on failure or managed by commit_remap on success. This prevents the
  /// infinite spin that occurred when MAP_FIXED targeted an address with
  /// an existing mapping table entry.
  bool begin_remap_guard(void *view_base, SIZE_T guarded_size) {
    if (!ensure_init())
      return false;

    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE) {
        uintptr_t expected = KEY_FREE;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          write_slot_replace_flags(slot, ViewSpec{nullptr, nullptr, {}, 0, 0}, 0);
          slot->extent = guarded_size;
          slot->remap_owner_thread = open_current_thread_handle();
          slot->remap_guard_index = claim_guard(slot);
          active_remap_count_.fetch_add(1, cpp::MemoryOrder::RELEASE);
          slot->key.store(target | REMAPPING_BIT,
                          cpp::MemoryOrder::RELEASE);
          return true;
        }
        continue;
      }

      if (k == target) {
        // LIVE entry at target address — take over for MAP_FIXED.
        // Handles stay in the slot: abort_remap_guard closes them on
        // failure; prepare_for_fixed won't find this entry (it calls
        // extract() which requires LIVE state, but we're REMAPPING).
        uintptr_t expected = target;
        if (slot->key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          slot->extent = guarded_size;
          slot->remap_owner_thread = open_current_thread_handle();
          slot->remap_guard_index = claim_guard(slot);
          active_remap_count_.fetch_add(1, cpp::MemoryOrder::RELEASE);
          slot->key.store(target | REMAPPING_BIT,
                          cpp::MemoryOrder::RELEASE);
          return true;
        }
        continue;
      }

      // Concurrent WRITING or REMAPPING on this address — wait.
      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }

      // Different address in this radix slot — should not occur (radix
      // tree guarantees unique address-to-slot mapping). Defensive bail.
      return false;
    }
  }

  /// REMAPPING -> WRITING|REMAPPING -> LIVE with new data. O(1).
  ///
  /// Handles in spec are duplicated unless they match the slot's current
  /// handles. Returns true on success. For different-base remaps, returns
  /// false if the new entry could not be inserted. The old slot is always
  /// cleaned up regardless.
  bool commit_remap(void *old_view_base, void *new_view_base,
                    SIZE_T view_size, ViewSpec spec) {
    uintptr_t old_target = reinterpret_cast<uintptr_t>(old_view_base);
    uintptr_t new_target = reinterpret_cast<uintptr_t>(new_view_base);

    Slot *old_slot = find_slot(old_target);
    if (LIBC_UNLIKELY(!old_slot))
      return false;

    uintptr_t k = old_slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (old_target | REMAPPING_BIT))
      return false;

    if (old_target == new_target) {
      // Same-base: update in place.
      old_slot->key.store(new_target | WRITING_BIT | REMAPPING_BIT,
                          cpp::MemoryOrder::RELEASE);

      bool same_file = (spec.file == old_slot->file_handle);
      bool same_section = (spec.section == old_slot->section_handle);

      ViewSpec commit_spec = spec;
      if (!same_file) {
        HANDLE new_file = dup_handle(spec.file);
        if (new_file) {
          if (old_slot->file_handle)
            ::NtClose(old_slot->file_handle);
          commit_spec.file = new_file;
        } else {
          commit_spec.file = old_slot->file_handle;
        }
      }
      if (!same_section) {
        HANDLE new_section = dup_handle(spec.section);
        if (new_section) {
          if (old_slot->section_handle)
            ::NtClose(old_slot->section_handle);
          commit_spec.section = new_section;
        } else {
          commit_spec.section = old_slot->section_handle;
        }
      }

      write_slot_accumulate_flags(old_slot, commit_spec, view_size);
      int gi = old_slot->remap_guard_index;
      close_owner_handle(old_slot);
      set_occupancy_bit(new_target); // Before publish: idempotent for
      publish_slot(old_slot, new_target); // LIVE→REMAP→LIVE; needed for
                                          // sentinel KEY_FREE→REMAP→LIVE.
      release_guard(gi);
      if (active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1)
        try_shrink_guards();
      return true;
    }

    // Different-base: insert at new radix position, free old.
    Slot *new_slot = ensure_slot(new_target);
    bool inserted = false;

    ViewSpec dup_spec = spec;
    dup_spec.file = dup_handle(spec.file);
    dup_spec.section = dup_handle(spec.section);

    uintptr_t expected = KEY_FREE;
    if (new_slot->key.compare_exchange_strong(
            expected, new_target | WRITING_BIT,
            cpp::MemoryOrder::ACQUIRE)) {
      write_slot_replace_flags(new_slot, dup_spec, view_size);
      set_occupancy_bit(new_target);
      publish_slot(new_slot, new_target);
      inserted = true;
    } else {
      if (dup_spec.file)
        ::NtClose(dup_spec.file);
      if (dup_spec.section)
        ::NtClose(dup_spec.section);
    }

    // Always clean up old slot.
    int gi = old_slot->remap_guard_index;
    close_slot_handles(old_slot);
    close_owner_handle(old_slot);
    publish_slot(old_slot, KEY_FREE);
    clear_occupancy_bit(old_target);
    release_guard(gi);
    if (active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1)
      try_shrink_guards();
    return inserted;
  }

  /// REMAPPING -> LIVE (restore original). O(1).
  void abort_remap(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (target | REMAPPING_BIT))
      return;

    int gi = slot->remap_guard_index;
    close_owner_handle(slot);
    publish_slot(slot, target);
    release_guard(gi);
    if (active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1)
      try_shrink_guards();
  }

  /// REMAPPING -> KEY_FREE. Closes handles. Irreversible failure. O(1).
  void discard_remap(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (target | REMAPPING_BIT))
      return;

    int gi = slot->remap_guard_index;
    close_slot_handles(slot);
    close_owner_handle(slot);
    publish_slot(slot, KEY_FREE);
    clear_occupancy_bit(target);
    release_guard(gi);
    if (active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1)
      try_shrink_guards();
  }

  /// REMAPPING sentinel -> KEY_FREE. O(1).
  ///
  /// Closes any handles left in the slot. For sentinels created from
  /// KEY_FREE, handles are null (close is a no-op). For sentinels that
  /// took over a LIVE slot, this properly frees the orphaned handles.
  void abort_remap_guard(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (target | REMAPPING_BIT))
      return;

    int gi = slot->remap_guard_index;
    close_slot_handles(slot);
    close_owner_handle(slot);
    publish_slot(slot, KEY_FREE);
    clear_occupancy_bit(target);
    release_guard(gi);
    if (active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1)
      try_shrink_guards();
  }

  // -----------------------------------------------------------------------
  // VEH remap guard interface
  // -----------------------------------------------------------------------

  /// Check if a fault address falls in any REMAPPING slot's guarded range.
  /// Returns guard index if found (caller calls wait_for_remap), -1 if not.
  /// Dead owners are recovered inline.
  /// O(high_water_mark_) — scans the guard array, not the radix tree.
  int check_remap_guard(uintptr_t fault_addr) {
    if (LIBC_UNLIKELY(!remap_guards_))
      return -1;
    // Fast-path: no concurrent remaps → skip entirely. Common case
    // for >99.99% of ACCESS_VIOLATION dispatches.
    if (LIBC_LIKELY(
            active_remap_count_.load(cpp::MemoryOrder::ACQUIRE) == 0))
      return -1;

    uint32_t hwm = guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    for (uint32_t i = 0; i < hwm; ++i) {
      uintptr_t ptr = remap_guards_[i].load(cpp::MemoryOrder::ACQUIRE);
      if (!ptr)
        continue;

      Slot *slot = reinterpret_cast<Slot *>(ptr);
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (!(k & REMAPPING_BIT))
        continue;

      uintptr_t base = k & ~STATE_MASK;
      SIZE_T size = slot->extent;

      if (fault_addr >= base && fault_addr < base + size) {
        // Re-validate REMAPPING state — the remap could have completed
        // between the range check above and now, freeing/reusing the slot
        // and invalidating remap_owner_thread.
        uintptr_t k_recheck = slot->key.load(cpp::MemoryOrder::ACQUIRE);
        if (!(k_recheck & REMAPPING_BIT))
          return -1; // Remap completed — not our concern anymore.
        if (is_thread_dead(slot->remap_owner_thread)) {
          recover_dead_remap(slot, k_recheck);
          return -1; // Dead owner → do NOT stall, deliver the fault.
        }
        return static_cast<int>(i); // Live owner → stall.
      }
    }
    return -1;
  }

  /// Wait for a REMAPPING slot to resolve. Bounded timeout with
  /// dead-owner recovery.
  void wait_for_remap(int guard_index) {
    uintptr_t ptr =
        remap_guards_[guard_index].load(cpp::MemoryOrder::ACQUIRE);
    if (!ptr)
      return; // Already resolved.

    Slot *slot = reinterpret_cast<Slot *>(ptr);
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (!(k & REMAPPING_BIT))
      return;

    LARGE_INTEGER timeout;
    timeout.QuadPart = REMAP_WAIT_TIMEOUT_100NS;

    while ((k = slot->key.load(cpp::MemoryOrder::ACQUIRE)) &
           REMAPPING_BIT) {
      futex_addr::wait_nt(
          reinterpret_cast<const volatile uintptr_t *>(&slot->key),
          k, &timeout);

      k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (!(k & REMAPPING_BIT))
        return;

      if (is_thread_dead(slot->remap_owner_thread)) {
        recover_dead_remap(slot, k);
        return;
      }
    }
  }

  // -----------------------------------------------------------------------
  // Fork reinit — run in the single-threaded fork child
  // -----------------------------------------------------------------------

  /// Clean up stale REMAPPING entries inherited from the parent.
  /// Single-threaded context. Scans the guard array (not the full
  /// radix tree) — O(high_water_mark_ at fork time), typically 0-1.
  void fork_reinit() {
    FutexValueType init_state = init_state_.load(cpp::MemoryOrder::ACQUIRE);
    if (init_state == INIT_UNINITIALIZED ||
        init_state == INIT_DESTROYED)
      return;

    if (init_state == INIT_IN_PROGRESS) {
      reset_root_state();
      init_state_.store(INIT_UNINITIALIZED, cpp::MemoryOrder::RELAXED);
      return;
    }

    // Phase 1: Clean up stale REMAPPING entries from the guard array.
    uint32_t hwm = guard_high_water_.load(cpp::MemoryOrder::RELAXED);
    for (uint32_t i = 0; i < hwm; ++i) {
      uintptr_t ptr = remap_guards_[i].load(cpp::MemoryOrder::RELAXED);
      if (!ptr)
        continue;

      Slot *slot = reinterpret_cast<Slot *>(ptr);
      uintptr_t k = slot->key.load(cpp::MemoryOrder::RELAXED);
      if (k & REMAPPING_BIT) {
        close_slot_handles(slot);
        close_owner_handle(slot);
        slot->version.fetch_add(1, cpp::MemoryOrder::RELAXED);
        slot->key.store(KEY_FREE, cpp::MemoryOrder::RELAXED);
        clear_occupancy_bit(k & ~STATE_MASK);
      }
      remap_guards_[i].store(0, cpp::MemoryOrder::RELAXED);
    }
    active_remap_count_.store(0, cpp::MemoryOrder::RELAXED);
    guard_high_water_.store(0, cpp::MemoryOrder::RELAXED);
    alloc_cursor_.store(0, cpp::MemoryOrder::RELAXED);

    // Phase 2: Duplicate section/file handles for LIVE slots.
    // After fork, parent and child share the same handle values. If the
    // parent munmaps a region (closing its section handle), the child's
    // inherited handle becomes invalid. Duplicate all LIVE handles so the
    // child holds independent kernel references.
    //
    // Bitmap-accelerated: scan the L3 occupancy bitmap (16 words = 2
    // cache lines) to skip empty slots instead of loading every slot's
    // key (1024 cache lines per L3Page).
    for (size_t i1 = 0; i1 < l1_size_; ++i1) {
      L2Page *l2 = l1_[i1].load(cpp::MemoryOrder::RELAXED);
      if (!l2)
        continue;
      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::RELAXED);
        if (!l3)
          continue;
        for (unsigned w = 0; w < 16; ++w) {
          uint64_t bits = l3->occupancy[w].load(cpp::MemoryOrder::RELAXED);
          while (bits) {
            unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
            unsigned i3 = w * 64 + bit;
            Slot *slot = &l3->slots[i3];
            uintptr_t k = slot->key.load(cpp::MemoryOrder::RELAXED);
            if (k != KEY_FREE && (k & STATE_MASK) == 0) {
              // LIVE slot: duplicate handles for independent references.
              slot->section_handle = dup_handle(slot->section_handle);
              slot->file_handle = dup_handle(slot->file_handle);
            }
            bits &= bits - 1;
          }
        }
      }
    }
  }

  // -----------------------------------------------------------------------
  // Bitmap-accelerated iteration
  // -----------------------------------------------------------------------

  /// Callback type for for_each_live. Receives a consistent snapshot
  /// (not a raw Slot* — Slot is internal). Function pointer + opaque
  /// context, matching the ReactorCallback pattern. No templates —
  /// avoids code bloat from the 5-level radix/bitmap loop.
  using MappingCallback = void (*)(const SlotSnapshot *snap, void *ctx);

  /// Count all live mappings. O(l1_size_ × L2 × 16) — scans occupancy bitmaps
  /// without touching slot cache lines.
  LIBC_INLINE unsigned count_live() {
    if (!ensure_init())
      return 0;
    unsigned count = 0;
    for (size_t i1 = 0; i1 < l1_size_; ++i1) {
      L2Page *l2 = l1_[i1].load(cpp::MemoryOrder::ACQUIRE);
      if (!l2)
        continue;
      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
        if (!l3)
          continue;
        for (unsigned w = 0; w < 16; ++w)
          count += static_cast<unsigned>(__builtin_popcountll(
              l3->occupancy[w].load(cpp::MemoryOrder::RELAXED)));
      }
    }
    return count;
  }

  /// Iterate all live mappings. Bitmap-accelerated: only touches cache
  /// lines of occupied slots. Each callback receives a seqlock-verified
  /// SlotSnapshot — consistent, ABA-safe, no internal types exposed.
  ///
  /// False positives from the bitmap (bit set, slot freed between bitmap
  /// read and key check) are filtered by the snapshot protocol. Prefetches
  /// the next slot while snapshotting the current one.
  void for_each_live(MappingCallback cb, void *ctx) {
    if (!ensure_init())
      return;
    for (size_t i1 = 0; i1 < l1_size_; ++i1) {
      L2Page *l2 = l1_[i1].load(cpp::MemoryOrder::ACQUIRE);
      if (!l2)
        continue;
      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
        if (!l3)
          continue;
        for (unsigned w = 0; w < 16; ++w) {
          uint64_t bits = l3->occupancy[w].load(cpp::MemoryOrder::ACQUIRE);
          while (bits) {
            unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
            unsigned i3 = w * 64 + bit;
            // Prefetch next live slot (scattered access — hw prefetcher
            // can't predict bitmap-driven patterns).
            uint64_t next_bits = bits & (bits - 1);
            if (LIBC_LIKELY(next_bits != 0)) {
              unsigned nb = static_cast<unsigned>(__builtin_ctzll(next_bits));
              __builtin_prefetch(&l3->slots[w * 64 + nb], 0, 1);
            }
            // Use the existing seqlock snapshot() for consistency.
            Slot *slot = &l3->slots[i3];
            uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
            if (k != KEY_FREE && (k & STATE_MASK) == 0) {
              SlotSnapshot snap;
              if (snapshot(reinterpret_cast<void *>(k), &snap))
                cb(&snap, ctx);
            }
            bits = next_bits;
          }
        }
      }
    }
  }

  /// Release the full mapping-table backing store. Safe only during process
  /// shutdown when no other threads can race with the table.
  void destroy() {
    if (init_state_.load(cpp::MemoryOrder::ACQUIRE) != INIT_READY)
      return;

    for (size_t i1 = 0; i1 < l1_size_; ++i1) {
      L2Page *l2 = l1_[i1].load(cpp::MemoryOrder::RELAXED);
      if (!l2)
        continue;

      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::RELAXED);
        if (!l3)
          continue;

        for (unsigned i3 = 0; i3 < static_cast<unsigned>(L3_SIZE); ++i3) {
          Slot *slot = &l3->slots[i3];
          uintptr_t k = slot->key.load(cpp::MemoryOrder::RELAXED);
          if (k == KEY_FREE)
            continue;

          close_slot_handles(slot);
          if (k & REMAPPING_BIT)
            close_owner_handle(slot);
          slot->flags.store(0, cpp::MemoryOrder::RELAXED);
          slot->version.fetch_add(1, cpp::MemoryOrder::RELAXED);
          slot->key.store(KEY_FREE, cpp::MemoryOrder::RELAXED);
        }

        internal::page_free(l3);
        l2->children[i2].store(nullptr, cpp::MemoryOrder::RELAXED);
      }

      internal::page_free(l2);
      l1_[i1].store(nullptr, cpp::MemoryOrder::RELAXED);
    }

    reset_root_state();
    init_state_.store_and_notify_all(INIT_DESTROYED);
  }
};

/// Global mapping table controller. Geometry is initialized lazily from the
/// runtime user VA ceiling recorded in the PCB.
inline MappingTable g_mapping_table;

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MAPPING_TABLE_H
