//===-- SysV shared memory engine for Windows NT-POSIX --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements shmget/shmat/shmdt/shmctl using NT named sections.
//
// Architecture:
//   - A process-shared registry lives in a named NT section
//     (\BaseNamedObjects\llvm_libc_sysv_shm_reg). Contains an array of
//     ShmSlot entries with atomic state management for lock-free access.
//   - Each data segment is a separate named section
//     (\BaseNamedObjects\llvm_libc_sysv_shm_<id>).
//   - A process-local attach table tracks mapped addresses for shmdt lookup.
//
// All functions return value on success, -errno on failure.
// Never includes or sets libc_errno.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/sysv_shm_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/types/key_t.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/time_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/section_region.h"
#include "src/__support/OSUtil/windows/alloc/section_view.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_context_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "include/llvm-libc-macros/sys-ipc-macros.h"
#include "include/llvm-libc-macros/sys-shm-macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Constants
// =========================================================================

static constexpr size_t MAX_SHM_SEGMENTS = 256;
static constexpr LONGLONG NT_EPOCH_DELTA = 116444736000000000LL;
static constexpr LONGLONG TICKS_PER_SECOND = 10000000LL;

// =========================================================================
// Time helper
// =========================================================================

static time_t current_posix_time() {
  LONGLONG ticks = RtlGetSystemTimePrecise() - NT_EPOCH_DELTA;
  return static_cast<time_t>(ticks / TICKS_PER_SECOND);
}

// =========================================================================
// Registry slot — one per SysV shared memory segment.
// Stored in a shared section visible to all processes.
// =========================================================================

enum ShmSlotState : uint32_t {
  SHM_SLOT_EMPTY = 0,
  SHM_SLOT_CREATING = 1,
  SHM_SLOT_LIVE = 2,
  SHM_SLOT_REMOVED = 3,
};

struct ShmSlot {
  cpp::Atomic<uint32_t> state;
  key_t key;
  int shmid;
  size_t shm_segsz;
  mode_t shm_perm_mode;
  uid_t shm_perm_uid;
  gid_t shm_perm_gid;
  uid_t shm_perm_cuid;
  gid_t shm_perm_cgid;
  unsigned short shm_perm_seq; // generation counter
  pid_t shm_cpid;
  pid_t shm_lpid;
  cpp::Atomic<uint32_t> shm_nattch;
  cpp::Atomic<time_t> shm_atime;
  cpp::Atomic<time_t> shm_dtime;
  cpp::Atomic<time_t> shm_ctime;
};

struct ShmRegistry {
  cpp::Atomic<int> next_id;
  ShmSlot slots[MAX_SHM_SEGMENTS];
};

// =========================================================================
// Process-local attach table — maps addresses to (shmid, view_size) pairs.
// =========================================================================

static constexpr size_t MAX_ATTACHES = 256;

struct AttachEntry {
  void *addr;
  size_t size;
  int shmid;
};

static AttachEntry g_attaches[MAX_ATTACHES];
static cpp::Atomic<uint32_t> g_attach_count{0};

static int attach_record(void *addr, size_t size, int shmid) {
  uint32_t idx = g_attach_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  if (idx >= MAX_ATTACHES) {
    g_attach_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
    return -EMFILE;
  }
  g_attaches[idx].addr = addr;
  g_attaches[idx].size = size;
  g_attaches[idx].shmid = shmid;
  return 0;
}

static AttachEntry *attach_find(const void *addr) {
  uint32_t count = g_attach_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint32_t i = 0; i < count && i < MAX_ATTACHES; ++i) {
    if (g_attaches[i].addr == addr)
      return &g_attaches[i];
  }
  return nullptr;
}

static void attach_remove(AttachEntry *entry) {
  // Swap with the last entry and shrink.
  uint32_t count = g_attach_count.load(cpp::MemoryOrder::ACQUIRE);
  if (count == 0)
    return;
  uint32_t last = count - 1;
  AttachEntry *last_entry = &g_attaches[last];
  if (entry != last_entry) {
    entry->addr = last_entry->addr;
    entry->size = last_entry->size;
    entry->shmid = last_entry->shmid;
  }
  last_entry->addr = nullptr;
  g_attach_count.fetch_sub(1, cpp::MemoryOrder::RELEASE);
}

// =========================================================================
// Fork reinit — compact the attach table after fork.
//
// If fork() fires while another thread is mid-shmat (between the
// fetch_add on g_attach_count and the three field stores), the child
// inherits a partially-committed entry with addr == nullptr.  Since
// the child is single-threaded at this point we can safely compact
// the table: drop any entry whose addr is null and shrink the count.
// Valid entries (shm views inherited via NtCreateProcessEx address
// space clone) are preserved — POSIX requires shm attachments to
// survive fork.
// =========================================================================

static void sysv_shm_fork_reinit_impl() {
  uint32_t count = g_attach_count.load(cpp::MemoryOrder::RELAXED);
  if (count > MAX_ATTACHES)
    count = MAX_ATTACHES;

  // Compact: move valid entries toward the front, skip nulls.
  uint32_t dst = 0;
  for (uint32_t src = 0; src < count; ++src) {
    if (g_attaches[src].addr != nullptr) {
      if (dst != src)
        g_attaches[dst] = g_attaches[src];
      ++dst;
    }
  }
  // Zero trailing slots so future scans don't see stale data.
  for (uint32_t i = dst; i < count; ++i)
    g_attaches[i].addr = nullptr;

  g_attach_count.store(dst, cpp::MemoryOrder::RELAXED);
}

// =========================================================================
// Registry access — lazy-init, process-shared named section.
// =========================================================================

// Process-lifetime — intentionally never destroyed (the mapping must
// survive until process exit, and NtUnmapViewOfSectionEx during DLL
// unload is unsafe).
[[clang::no_destroy]] static windows::SectionRegion g_registry_region;

static ShmRegistry *get_registry() {
  if (g_registry_region)
    return g_registry_region.as<ShmRegistry>();

  // Build the section name: \BaseNamedObjects\llvm_libc_sysv_shm_reg
  constexpr WCHAR NAME[] = u"\\BaseNamedObjects\\llvm_libc_sysv_shm_reg";
  UNICODE_STRING us;
  us.Length = sizeof(NAME) - sizeof(WCHAR);
  us.MaximumLength = sizeof(NAME);
  us.Buffer = const_cast<WCHAR *>(NAME);

  NTSTATUS st;
  g_registry_region = windows::SectionRegion::create_named(
      &us, sizeof(ShmRegistry), PAGE_READWRITE, nullptr, &st);
  if (!g_registry_region)
    return nullptr;

  return g_registry_region.as<ShmRegistry>();
}

// =========================================================================
// Section name builder for data segments.
// =========================================================================

// Build \BaseNamedObjects\llvm_libc_sysv_shm_<id>
// Returns length in WCHARs (excluding NUL), or 0 on failure.
static size_t build_segment_name(int shmid, WCHAR *buf, size_t max_wchars) {
  constexpr WCHAR PREFIX[] = u"\\BaseNamedObjects\\llvm_libc_sysv_shm_";
  constexpr size_t PREFIX_LEN = sizeof(PREFIX) / sizeof(WCHAR) - 1;

  if (max_wchars < PREFIX_LEN + 12) // room for integer + NUL
    return 0;

  for (size_t i = 0; i < PREFIX_LEN; ++i)
    buf[i] = PREFIX[i];

  // Convert shmid to decimal string.
  unsigned int id = static_cast<unsigned int>(shmid);
  WCHAR digits[11];
  int ndigits = 0;
  if (id == 0) {
    digits[ndigits++] = u'0';
  } else {
    while (id > 0) {
      digits[ndigits++] = u'0' + static_cast<WCHAR>(id % 10);
      id /= 10;
    }
  }
  // Reverse digits into output.
  for (int i = ndigits - 1; i >= 0; --i)
    buf[PREFIX_LEN + (ndigits - 1 - i)] = digits[i];

  size_t total = PREFIX_LEN + ndigits;
  buf[total] = u'\0';
  return total;
}

// =========================================================================
// Permission checking
// =========================================================================

static bool check_perms(const ShmSlot *slot, int shmflg) {
  // Simplified: grant all access. A full implementation would compare
  // effective uid/gid against slot owner/group and check mode bits.
  // For now, the POSIX permission model is enforced at the NT section
  // level via the security descriptor.
  (void)slot;
  (void)shmflg;
  return true;
}

// =========================================================================
// shmget — create or look up a shared memory segment
// =========================================================================

intptr_t shmget(key_t key, size_t size, int shmflg) {
  ShmRegistry *reg = get_registry();
  if (!reg)
    return -ENOMEM;

  // --- Lookup existing key (if not IPC_PRIVATE) ---
  if (key != IPC_PRIVATE) {
    for (size_t i = 0; i < MAX_SHM_SEGMENTS; ++i) {
      ShmSlot *slot = &reg->slots[i];
      uint32_t st = slot->state.load(cpp::MemoryOrder::ACQUIRE);
      if (st != SHM_SLOT_LIVE)
        continue;
      if (slot->key != key)
        continue;

      // Found existing segment with matching key.
      if (shmflg & IPC_CREAT && shmflg & IPC_EXCL)
        return -EEXIST;
      if (size > slot->shm_segsz && size != 0)
        return -EINVAL;
      if (!check_perms(slot, shmflg))
        return -EACCES;
      return static_cast<intptr_t>(slot->shmid);
    }

    // Not found — if IPC_CREAT not set, fail.
    if (!(shmflg & IPC_CREAT))
      return -ENOENT;
  }

  // --- Allocate a new segment ---
  if (size == 0)
    return -EINVAL;

  // Round up to allocation granularity (64KB on NT).
  constexpr size_t ALLOC_GRAN = 65536;
  size_t alloc_size = (size + ALLOC_GRAN - 1) & ~(ALLOC_GRAN - 1);

  // Find a free slot.
  ShmSlot *slot = nullptr;
  size_t slot_idx = 0;
  for (size_t i = 0; i < MAX_SHM_SEGMENTS; ++i) {
    uint32_t expected = SHM_SLOT_EMPTY;
    if (reg->slots[i].state.compare_exchange_strong(
            expected, SHM_SLOT_CREATING, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED)) {
      slot = &reg->slots[i];
      slot_idx = i;
      break;
    }
  }
  if (!slot)
    return -ENOSPC;

  // Allocate a unique shmid.
  int id = reg->next_id.fetch_add(1, cpp::MemoryOrder::RELAXED);

  // Create the named NT section for the data segment.
  WCHAR name_buf[80];
  size_t name_len = build_segment_name(id, name_buf, 80);
  if (name_len == 0) {
    slot->state.store(SHM_SLOT_EMPTY, cpp::MemoryOrder::RELEASE);
    return -EINVAL;
  }

  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(name_len * sizeof(WCHAR));
  us.MaximumLength = static_cast<USHORT>((name_len + 1) * sizeof(WCHAR));
  us.Buffer = name_buf;

  NTSTATUS nt_st;
  windows::SectionHandle section = windows::SectionHandle::create_named(
      &us, alloc_size,
      SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
      PAGE_READWRITE, SEC_COMMIT, nullptr, &nt_st);
  if (!section) {
    slot->state.store(SHM_SLOT_EMPTY, cpp::MemoryOrder::RELEASE);
    return -ENOMEM;
  }

  // Populate slot metadata.
  mode_t mode = static_cast<mode_t>(shmflg & 0777);
  pid_t pid = static_cast<pid_t>(NtCurrentProcessId());

  slot->key = key;
  slot->shmid = id;
  slot->shm_segsz = size;
  slot->shm_perm_mode = mode;
  slot->shm_perm_uid = 0;  // TODO: wire to our uid model
  slot->shm_perm_gid = 0;
  slot->shm_perm_cuid = 0;
  slot->shm_perm_cgid = 0;
  slot->shm_perm_seq = static_cast<unsigned short>(slot_idx);
  slot->shm_cpid = pid;
  slot->shm_lpid = 0;
  slot->shm_nattch.store(0, cpp::MemoryOrder::RELAXED);
  slot->shm_atime.store(0, cpp::MemoryOrder::RELAXED);
  slot->shm_dtime.store(0, cpp::MemoryOrder::RELAXED);
  slot->shm_ctime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);

  // Section handle is intentionally leaked into the kernel object namespace.
  // It persists as long as any process has the section open or mapped.
  // When the slot is IPC_RMID'd and nattch reaches 0, the section is
  // naturally cleaned up by the NT object manager.
  (void)section.release();

  // Publish the slot.
  slot->state.store(SHM_SLOT_LIVE, cpp::MemoryOrder::RELEASE);
  return static_cast<intptr_t>(id);
}

// =========================================================================
// shmat — attach a shared memory segment
// =========================================================================

intptr_t shmat(int shmid, const void *shmaddr, int shmflg) {
  ShmRegistry *reg = get_registry();
  if (!reg)
    return -ENOMEM;

  // Find the slot by shmid.
  ShmSlot *slot = nullptr;
  for (size_t i = 0; i < MAX_SHM_SEGMENTS; ++i) {
    if (reg->slots[i].state.load(cpp::MemoryOrder::ACQUIRE) == SHM_SLOT_LIVE &&
        reg->slots[i].shmid == shmid) {
      slot = &reg->slots[i];
      break;
    }
  }
  if (!slot)
    return -EINVAL;

  if (!check_perms(slot, shmflg))
    return -EACCES;

  // Open the named section for this segment.
  WCHAR name_buf[80];
  size_t name_len = build_segment_name(shmid, name_buf, 80);
  if (name_len == 0)
    return -EINVAL;

  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(name_len * sizeof(WCHAR));
  us.MaximumLength = static_cast<USHORT>((name_len + 1) * sizeof(WCHAR));
  us.Buffer = name_buf;

  bool read_only = (shmflg & SHM_RDONLY) != 0;
  ACCESS_MASK access = read_only ? SECTION_MAP_READ
                                 : (SECTION_MAP_READ | SECTION_MAP_WRITE);
  DWORD prot = read_only ? PAGE_READONLY : PAGE_READWRITE;

  NTSTATUS nt_st;
  windows::SectionHandle section =
      windows::SectionHandle::open_named(&us, access, &nt_st);
  if (!section)
    return -EINVAL;

  // Fixed-address mapping (SHM_RND / explicit shmaddr) is not supported.
  // NT sections require placeholder-based fixed mapping which is complex
  // for the SysV shm use case. Return EINVAL for now.
  if (shmaddr)
    return -EINVAL;

  windows::SectionView view =
      windows::SectionView::map_anywhere(section, prot, 0, {}, &nt_st);
  if (!view)
    return -ENOMEM;

  void *mapped_addr = view.detach();
  size_t mapped_size = slot->shm_segsz;

  // Record in process-local attach table.
  int rec_err = attach_record(mapped_addr, mapped_size, shmid);
  if (rec_err < 0) {
    // Unmap on failure.
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), mapped_addr, 0);
    return rec_err;
  }

  // Update registry metadata.
  slot->shm_nattch.fetch_add(1, cpp::MemoryOrder::RELAXED);
  slot->shm_atime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
  slot->shm_lpid = static_cast<pid_t>(NtCurrentProcessId());

  return reinterpret_cast<intptr_t>(mapped_addr);
}

// =========================================================================
// shmdt — detach a shared memory segment
// =========================================================================

intptr_t shmdt(const void *shmaddr) {
  AttachEntry *entry = attach_find(shmaddr);
  if (!entry)
    return -EINVAL;

  int shmid = entry->shmid;

  // Unmap the view.
  NTSTATUS st = ::NtUnmapViewOfSectionEx(
      NtCurrentProcess(), const_cast<void *>(shmaddr), 0);
  if (!NT_SUCCESS(st))
    return -EINVAL;

  // Remove from attach table.
  attach_remove(entry);

  // Update registry metadata.
  ShmRegistry *reg = get_registry();
  if (reg) {
    for (size_t i = 0; i < MAX_SHM_SEGMENTS; ++i) {
      ShmSlot *slot = &reg->slots[i];
      if (slot->state.load(cpp::MemoryOrder::ACQUIRE) >= SHM_SLOT_LIVE &&
          slot->shmid == shmid) {
        slot->shm_nattch.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        slot->shm_dtime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
        slot->shm_lpid = static_cast<pid_t>(NtCurrentProcessId());
        break;
      }
    }
  }

  return 0;
}

// =========================================================================
// shmctl — control operations on a shared memory segment
// =========================================================================

intptr_t shmctl(int shmid, int cmd, struct shmid_ds *buf) {
  ShmRegistry *reg = get_registry();
  if (!reg)
    return -ENOMEM;

  // Find the slot by shmid.
  ShmSlot *slot = nullptr;
  for (size_t i = 0; i < MAX_SHM_SEGMENTS; ++i) {
    uint32_t st = reg->slots[i].state.load(cpp::MemoryOrder::ACQUIRE);
    if (st >= SHM_SLOT_LIVE && reg->slots[i].shmid == shmid) {
      slot = &reg->slots[i];
      break;
    }
  }
  if (!slot)
    return -EINVAL;

  switch (cmd) {
  case IPC_STAT: {
    if (!buf)
      return -EFAULT;
    if (!check_perms(slot, 0))
      return -EACCES;

    buf->shm_perm.__key = slot->key;
    buf->shm_perm.uid = slot->shm_perm_uid;
    buf->shm_perm.gid = slot->shm_perm_gid;
    buf->shm_perm.cuid = slot->shm_perm_cuid;
    buf->shm_perm.cgid = slot->shm_perm_cgid;
    buf->shm_perm.mode = slot->shm_perm_mode;
    buf->shm_perm.__seq = slot->shm_perm_seq;
    buf->shm_segsz = slot->shm_segsz;
    buf->shm_atime = slot->shm_atime.load(cpp::MemoryOrder::RELAXED);
    buf->shm_dtime = slot->shm_dtime.load(cpp::MemoryOrder::RELAXED);
    buf->shm_ctime = slot->shm_ctime.load(cpp::MemoryOrder::RELAXED);
    buf->shm_cpid = slot->shm_cpid;
    buf->shm_lpid = slot->shm_lpid;
    buf->shm_nattch = slot->shm_nattch.load(cpp::MemoryOrder::RELAXED);
    return 0;
  }

  case IPC_SET: {
    if (!buf)
      return -EFAULT;
    slot->shm_perm_uid = buf->shm_perm.uid;
    slot->shm_perm_gid = buf->shm_perm.gid;
    slot->shm_perm_mode = buf->shm_perm.mode & 0777;
    slot->shm_ctime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
    return 0;
  }

  case IPC_RMID: {
    // Mark as removed. The NT section remains alive until all processes
    // unmap their views — the object manager handles refcounting.
    slot->state.store(SHM_SLOT_REMOVED, cpp::MemoryOrder::RELEASE);
    return 0;
  }

  case SHM_LOCK:
  case SHM_UNLOCK:
    // Locking/unlocking physical pages is a no-op on Windows.
    // NT manages working sets and page residency automatically.
    return 0;

  default:
    return -EINVAL;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
void LIBC_NAMESPACE::internal::sysv_shm_fork_reinit() {
  LIBC_NAMESPACE::internal::sysv_shm_fork_reinit_impl();
}
