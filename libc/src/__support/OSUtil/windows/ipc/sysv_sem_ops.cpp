//===-- SysV semaphore engine for Windows NT-POSIX ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements semget/semctl/semop using:
//   - a process-shared registry section for key -> semid lookup and metadata
//   - one named section per semaphore set for values / wait counts / undo data
//   - one named mutant per semaphore set for cross-process serialization
//
// All functions return value on success, -errno on failure.
// Never includes or sets libc_errno directly.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/sysv_sem_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_semid_ds.h"
#include "hdr/types/time_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/section_region.h"
#include "src/__support/OSUtil/windows/alloc/legacy/section_view.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/nt/session_bno.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getegid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/geteuid.h"
#include "src/__support/CPP/span.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/macros/config.h"

#include "include/llvm-libc-macros/sys-ipc-macros.h"
#include "include/llvm-libc-macros/sys-sem-macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

static constexpr size_t MAX_SEM_SETS = 256;
static constexpr size_t MAX_SEM_CACHE = 512;
static constexpr int MAX_SEMS_PER_SET = 256;
static constexpr size_t MAX_UNDO_SLOTS = 64;
static constexpr int SEM_VALUE_MAXIMUM = 32767;
static constexpr uint32_t SEM_SECTION_MAGIC = 0x53454d31; // "SEM1"
static constexpr uint32_t SEM_SECTION_VERSION = 1;
static constexpr LONGLONG NT_EPOCH_DELTA = 116444736000000000LL;
static constexpr LONGLONG TICKS_PER_SECOND = 10000000LL;
static constexpr LONGLONG CACHE_WAIT_INTERVAL_100NS = -10000LL; // 1ms
static constexpr LONGLONG SEM_WAIT_INTERVAL_100NS = -100000LL;  // 10ms

enum SemSlotState : uint32_t {
  SEM_SLOT_EMPTY = 0,
  SEM_SLOT_CREATING = 1,
  SEM_SLOT_LIVE = 2,
  SEM_SLOT_REMOVED = 3,
};

struct SemSlot {
  cpp::Atomic<uint32_t> state;
  key_t key;
  int semid;
  int nsems;
  mode_t sem_perm_mode;
  uid_t sem_perm_uid;
  gid_t sem_perm_gid;
  uid_t sem_perm_cuid;
  gid_t sem_perm_cgid;
  unsigned short sem_perm_seq;
  pid_t sem_cpid;
  cpp::Atomic<time_t> sem_otime;
  cpp::Atomic<time_t> sem_ctime;
};

struct SemRegistry {
  cpp::Atomic<int> next_id;
  SemSlot slots[MAX_SEM_SETS];
};

struct SemSetHeader {
  uint32_t magic;
  uint32_t version;
  int semid;
  int nsems;
  uint32_t undo_capacity;
  uint32_t reserved;
};

struct SemValue {
  cpp::Atomic<int> value;
  cpp::Atomic<pid_t> last_pid;
  cpp::Atomic<uint32_t> wait_negative;
  cpp::Atomic<uint32_t> wait_zero;
};

struct UndoSlot {
  cpp::Atomic<uint32_t> state;
  pid_t pid;
};

struct CachedSemSet {
  int semid = -1;
  int nsems = 0;
  HANDLE section = nullptr;
  HANDLE lock = nullptr;
  SemSetHeader *shared = nullptr;
};

struct BlockedOp {
  int sem_index = -1;
  bool wait_zero = false;
};

[[clang::no_destroy]] static windows::SectionRegion g_registry_region;
static cpp::Atomic<uint32_t> g_cache_lock{0};
static CachedSemSet g_cached_sets[MAX_SEM_CACHE];

static time_t current_posix_time() {
  LONGLONG ticks = RtlGetSystemTimePrecise() - NT_EPOCH_DELTA;
  return static_cast<time_t>(ticks / TICKS_PER_SECOND);
}

static void sleep_for_100ns(LONGLONG ticks) {
  LARGE_INTEGER interval;
  interval.QuadPart = ticks;
  (void)::NtDelayExecution(FALSE, &interval);
}

static void lock_cache() {
  while (true) {
    uint32_t expected = 0;
    if (g_cache_lock.compare_exchange_strong(expected, 1,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::RELAXED))
      return;
    sleep_for_100ns(CACHE_WAIT_INTERVAL_100NS);
  }
}

static void unlock_cache() {
  g_cache_lock.store(0, cpp::MemoryOrder::RELEASE);
}

static SemRegistry *get_registry() {
  if (g_registry_region) {
    SemRegistry *reg = g_registry_region.as<SemRegistry>();
    if (reg->next_id.load(cpp::MemoryOrder::RELAXED) == 0)
      reg->next_id.store(1, cpp::MemoryOrder::RELAXED);
    return reg;
  }

  auto name_s = windows::path_scratch();
  if (!name_s)
    return nullptr;
  windows::WStringStream ss(cpp::span<WCHAR>(name_s.data(), name_s.size() - 1));
  windows::write_session_bno_prefix(ss);
  ss << u"llvm_libc_sysv_sem_reg";
  if (ss.overflow())
    return nullptr;
  ss.null_terminate();
  windows::nt_wstring_view reg_wsv(name_s.data(), ss.str().size());

  NTSTATUS st;
  g_registry_region = windows::SectionRegion::create_named(
      &reg_wsv, sizeof(SemRegistry), PAGE_READWRITE, nullptr, &st);
  if (!g_registry_region)
    return nullptr;

  SemRegistry *reg = g_registry_region.as<SemRegistry>();
  if (reg->next_id.load(cpp::MemoryOrder::RELAXED) == 0)
    reg->next_id.store(1, cpp::MemoryOrder::RELAXED);
  return reg;
}

static size_t build_set_name(int semid, WCHAR *buf, size_t max_wchars) {
  windows::WStringStream ss(cpp::span<WCHAR>(buf, max_wchars - 1));
  windows::write_session_bno_prefix(ss);
  ss << u"llvm_libc_sysv_sem_" << static_cast<unsigned int>(semid);
  if (ss.overflow())
    return 0;
  ss.null_terminate();
  return ss.str().size();
}

static size_t build_lock_name(int semid, WCHAR *buf, size_t max_wchars) {
  windows::WStringStream ss(cpp::span<WCHAR>(buf, max_wchars - 1));
  windows::write_session_bno_prefix(ss);
  ss << u"llvm_libc_sysv_sem_lock_" << static_cast<unsigned int>(semid);
  if (ss.overflow())
    return 0;
  ss.null_terminate();
  return ss.str().size();
}

static size_t set_mapping_size(int nsems) {
  return sizeof(SemSetHeader) + sizeof(SemValue) * static_cast<size_t>(nsems) +
         sizeof(UndoSlot) * MAX_UNDO_SLOTS +
         sizeof(int) * MAX_UNDO_SLOTS * static_cast<size_t>(nsems);
}

static SemValue *sem_values(SemSetHeader *set) {
  return reinterpret_cast<SemValue *>(set + 1);
}

static UndoSlot *undo_slots(SemSetHeader *set) {
  return reinterpret_cast<UndoSlot *>(sem_values(set) + set->nsems);
}

static int *undo_adjustments(SemSetHeader *set, size_t slot_index) {
  return reinterpret_cast<int *>(undo_slots(set) + MAX_UNDO_SLOTS) +
         slot_index * static_cast<size_t>(set->nsems);
}

static HANDLE create_or_open_named_mutant(int semid) {
  auto name_s = windows::path_scratch();
  if (!name_s)
    return nullptr;
  size_t name_len = build_lock_name(semid, name_s.data(), name_s.size());
  if (name_len == 0)
    return nullptr;

  windows::nt_wstring_view lock_wsv(name_s.data(), name_len);

  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, lock_wsv.unicode_string(), OBJ_CASE_INSENSITIVE, nullptr, nullptr);

  HANDLE lock = nullptr;
  NTSTATUS st = ::NtCreateMutant(&lock, MUTANT_ALL_ACCESS, &oa, FALSE);
  return NT_SUCCESS(st) ? lock : nullptr;
}

static bool open_existing_set(int semid, int nsems, HANDLE *section_out,
                              SemSetHeader **shared_out) {
  auto name_s = windows::path_scratch();
  if (!name_s)
    return false;
  size_t name_len = build_set_name(semid, name_s.data(), name_s.size());
  if (name_len == 0)
    return false;

  windows::nt_wstring_view set_wsv(name_s.data(), name_len);

  NTSTATUS st;
  windows::SectionHandle section = windows::SectionHandle::open_named(
      &set_wsv, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY, &st);
  if (!section)
    return false;

  windows::SectionView view = windows::SectionView::map_anywhere(
      section, PAGE_READWRITE, set_mapping_size(nsems), {}, &st);
  if (!view)
    return false;

  SemSetHeader *shared = view.as<SemSetHeader>();
  if (shared->magic != SEM_SECTION_MAGIC || shared->version != SEM_SECTION_VERSION ||
      shared->semid != semid || shared->nsems != nsems) {
    view.unmap_release();
    return false;
  }

  *shared_out = static_cast<SemSetHeader *>(view.detach());
  *section_out = section.release();
  return true;
}

static bool create_new_set(int semid, int nsems, HANDLE *section_out,
                           SemSetHeader **shared_out) {
  auto name_s = windows::path_scratch();
  if (!name_s)
    return false;
  size_t name_len = build_set_name(semid, name_s.data(), name_s.size());
  if (name_len == 0)
    return false;

  windows::nt_wstring_view set_wsv(name_s.data(), name_len);

  NTSTATUS st;
  windows::SectionHandle section = windows::SectionHandle::create_named(
      &set_wsv, set_mapping_size(nsems),
      SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY, PAGE_READWRITE,
      SEC_COMMIT, nullptr, &st);
  if (!section)
    return false;

  windows::SectionView view = windows::SectionView::map_anywhere(
      section, PAGE_READWRITE, set_mapping_size(nsems), {}, &st);
  if (!view)
    return false;

  SemSetHeader *shared = view.as<SemSetHeader>();
  shared->magic = SEM_SECTION_MAGIC;
  shared->version = SEM_SECTION_VERSION;
  shared->semid = semid;
  shared->nsems = nsems;
  shared->undo_capacity = MAX_UNDO_SLOTS;
  shared->reserved = 0;

  SemValue *values = sem_values(shared);
  for (int i = 0; i < nsems; ++i) {
    values[i].value.store(0, cpp::MemoryOrder::RELAXED);
    values[i].last_pid.store(0, cpp::MemoryOrder::RELAXED);
    values[i].wait_negative.store(0, cpp::MemoryOrder::RELAXED);
    values[i].wait_zero.store(0, cpp::MemoryOrder::RELAXED);
  }

  UndoSlot *slots = undo_slots(shared);
  for (size_t i = 0; i < MAX_UNDO_SLOTS; ++i) {
    slots[i].state.store(0, cpp::MemoryOrder::RELAXED);
    slots[i].pid = 0;
    int *adjustments = undo_adjustments(shared, i);
    for (int sem_index = 0; sem_index < nsems; ++sem_index)
      adjustments[sem_index] = 0;
  }

  *shared_out = static_cast<SemSetHeader *>(view.detach());
  *section_out = section.release();
  return true;
}

static SemSlot *find_slot_by_semid(SemRegistry *reg, int semid) {
  for (size_t i = 0; i < MAX_SEM_SETS; ++i) {
    uint32_t state = reg->slots[i].state.load(cpp::MemoryOrder::ACQUIRE);
    if (state >= SEM_SLOT_LIVE && reg->slots[i].semid == semid)
      return &reg->slots[i];
  }
  return nullptr;
}

static CachedSemSet *get_cached_set(int semid, int nsems) {
  lock_cache();

  for (size_t i = 0; i < MAX_SEM_CACHE; ++i) {
    if (g_cached_sets[i].semid == semid) {
      CachedSemSet *cached = &g_cached_sets[i];
      unlock_cache();
      return cached;
    }
  }

  size_t free_index = MAX_SEM_CACHE;
  for (size_t i = 0; i < MAX_SEM_CACHE; ++i) {
    if (g_cached_sets[i].semid == -1) {
      free_index = i;
      break;
    }
  }

  if (free_index == MAX_SEM_CACHE) {
    unlock_cache();
    return nullptr;
  }

  HANDLE section = nullptr;
  SemSetHeader *shared = nullptr;
  if (!open_existing_set(semid, nsems, &section, &shared)) {
    unlock_cache();
    return nullptr;
  }
  windows::ScopedNtHandle section_owned(section);

  windows::ScopedNtHandle lock_owned(create_or_open_named_mutant(semid));
  if (!lock_owned) {
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), shared, 0);
    unlock_cache();
    return nullptr;
  }

  CachedSemSet &entry = g_cached_sets[free_index];
  entry.semid = semid;
  entry.nsems = nsems;
  entry.section = section_owned.release();
  entry.lock = lock_owned.release();
  entry.shared = shared;
  unlock_cache();
  return &entry;
}

static bool acquire_set_lock(HANDLE lock) {
  NTSTATUS st = ::NtWaitForSingleObject(lock, FALSE, nullptr);
  return NT_SUCCESS(st);
}

static void release_set_lock(HANDLE lock) {
  (void)::NtReleaseMutant(lock, nullptr);
}

static bool process_is_alive(pid_t pid) {
  if (pid <= 0)
    return false;

  windows::ScopedNtHandle process;
  NTSTATUS st = ::NtOpenProcessById(
      process.put(), PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
      static_cast<DWORD>(pid));
  if (!NT_SUCCESS(st))
    return false;

  LARGE_INTEGER zero_timeout;
  zero_timeout.QuadPart = 0;
  NTSTATUS wait_st =
      ::NtWaitForSingleObject(process.get(), FALSE, &zero_timeout);
  return wait_st == STATUS_TIMEOUT;
}

static void reap_dead_undo_slots(SemSlot *slot, SemSetHeader *set) {
  UndoSlot *slots = undo_slots(set);
  SemValue *values = sem_values(set);
  bool touched = false;

  for (size_t undo_index = 0; undo_index < MAX_UNDO_SLOTS; ++undo_index) {
    if (slots[undo_index].state.load(cpp::MemoryOrder::ACQUIRE) == 0)
      continue;

    pid_t pid = slots[undo_index].pid;
    if (process_is_alive(pid))
      continue;

    int *adjustments = undo_adjustments(set, undo_index);
    for (int sem_index = 0; sem_index < set->nsems; ++sem_index) {
      int delta = adjustments[sem_index];
      if (delta == 0)
        continue;

      int current = values[sem_index].value.load(cpp::MemoryOrder::RELAXED);
      long long next = static_cast<long long>(current) + delta;
      if (next < 0)
        next = 0;
      if (next > SEM_VALUE_MAXIMUM)
        next = SEM_VALUE_MAXIMUM;
      values[sem_index].value.store(static_cast<int>(next),
                                    cpp::MemoryOrder::RELAXED);
      values[sem_index].last_pid.store(pid, cpp::MemoryOrder::RELAXED);
      adjustments[sem_index] = 0;
      touched = true;
    }

    slots[undo_index].pid = 0;
    slots[undo_index].state.store(0, cpp::MemoryOrder::RELEASE);
  }

  if (touched)
    slot->sem_otime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
}

static bool check_perms(const SemSlot *slot, int semflg) {
  (void)slot;
  (void)semflg;
  return true;
}

static int get_or_create_undo_slot_index(SemSetHeader *set, pid_t pid) {
  UndoSlot *slots = undo_slots(set);
  int free_slot = -1;
  for (size_t i = 0; i < MAX_UNDO_SLOTS; ++i) {
    if (slots[i].state.load(cpp::MemoryOrder::ACQUIRE) != 0) {
      if (slots[i].pid == pid)
        return static_cast<int>(i);
      continue;
    }
    if (free_slot < 0)
      free_slot = static_cast<int>(i);
  }

  if (free_slot < 0)
    return -1;

  slots[free_slot].pid = pid;
  slots[free_slot].state.store(1, cpp::MemoryOrder::RELEASE);
  int *adjustments = undo_adjustments(set, static_cast<size_t>(free_slot));
  for (int sem_index = 0; sem_index < set->nsems; ++sem_index)
    adjustments[sem_index] = 0;
  return free_slot;
}

static void clear_undo_for_sem(SemSetHeader *set, int semnum) {
  UndoSlot *slots = undo_slots(set);
  for (size_t undo_index = 0; undo_index < MAX_UNDO_SLOTS; ++undo_index) {
    if (slots[undo_index].state.load(cpp::MemoryOrder::ACQUIRE) == 0)
      continue;
    undo_adjustments(set, undo_index)[semnum] = 0;
  }
}

static void clear_all_undo(SemSetHeader *set) {
  UndoSlot *slots = undo_slots(set);
  for (size_t undo_index = 0; undo_index < MAX_UNDO_SLOTS; ++undo_index) {
    if (slots[undo_index].state.load(cpp::MemoryOrder::ACQUIRE) == 0)
      continue;
    int *adjustments = undo_adjustments(set, undo_index);
    for (int sem_index = 0; sem_index < set->nsems; ++sem_index)
      adjustments[sem_index] = 0;
  }
}

static int try_semop_locked(SemSlot *slot, SemSetHeader *set,
                            struct sembuf *sops, size_t nsops, pid_t pid,
                            BlockedOp *blocked) {
  if (blocked)
    *blocked = {};

  SemValue *values = sem_values(set);
  int nsems = set->nsems;
  internal::ScratchAlloc<int> snapshots_s(nsems);
  internal::ScratchAlloc<bool> touched_s(nsems);
  internal::ScratchAlloc<int> undo_delta_s(nsems);
  if (!snapshots_s || !touched_s || !undo_delta_s)
    return -ENOMEM;
  int *snapshots = snapshots_s.data();
  bool *touched = touched_s.data();
  int *undo_delta = undo_delta_s.data();
  __builtin_memset(snapshots, 0, nsems * sizeof(int));
  __builtin_memset(touched, 0, nsems * sizeof(bool));
  __builtin_memset(undo_delta, 0, nsems * sizeof(int));

  for (int sem_index = 0; sem_index < set->nsems; ++sem_index)
    snapshots[sem_index] =
        values[sem_index].value.load(cpp::MemoryOrder::RELAXED);

  bool needs_undo = false;
  for (size_t op_index = 0; op_index < nsops; ++op_index) {
    const struct sembuf &op = sops[op_index];
    if (op.sem_num >= static_cast<unsigned short>(set->nsems))
      return -EFBIG;

    int sem_index = static_cast<int>(op.sem_num);
    int current = snapshots[sem_index];

    if (op.sem_op > 0) {
      if (current > SEM_VALUE_MAXIMUM - op.sem_op)
        return -ERANGE;
      snapshots[sem_index] = current + op.sem_op;
      touched[sem_index] = true;
    } else if (op.sem_op == 0) {
      if (current != 0) {
        if (op.sem_flg & IPC_NOWAIT)
          return -EAGAIN;
        if (blocked) {
          blocked->sem_index = sem_index;
          blocked->wait_zero = true;
        }
        return -EAGAIN;
      }
    } else {
      int need = -op.sem_op;
      if (current < need) {
        if (op.sem_flg & IPC_NOWAIT)
          return -EAGAIN;
        if (blocked) {
          blocked->sem_index = sem_index;
          blocked->wait_zero = false;
        }
        return -EAGAIN;
      }
      snapshots[sem_index] = current + op.sem_op;
      touched[sem_index] = true;
    }

    if (op.sem_flg & SEM_UNDO) {
      needs_undo = true;
      undo_delta[sem_index] -= op.sem_op;
    }
  }

  int undo_slot_index = -1;
  if (needs_undo) {
    undo_slot_index = get_or_create_undo_slot_index(set, pid);
    if (undo_slot_index < 0)
      return -ENOSPC;

    int *adjustments =
        undo_adjustments(set, static_cast<size_t>(undo_slot_index));
    for (int sem_index = 0; sem_index < set->nsems; ++sem_index) {
      if (undo_delta[sem_index] == 0)
        continue;
      long long next =
          static_cast<long long>(adjustments[sem_index]) + undo_delta[sem_index];
      if (next < -SEM_VALUE_MAXIMUM || next > SEM_VALUE_MAXIMUM)
        return -ERANGE;
    }
  }

  for (int sem_index = 0; sem_index < set->nsems; ++sem_index) {
    if (!touched[sem_index])
      continue;
    values[sem_index].value.store(snapshots[sem_index],
                                  cpp::MemoryOrder::RELAXED);
    values[sem_index].last_pid.store(pid, cpp::MemoryOrder::RELAXED);
  }

  if (needs_undo) {
    int *adjustments =
        undo_adjustments(set, static_cast<size_t>(undo_slot_index));
    for (int sem_index = 0; sem_index < set->nsems; ++sem_index)
      adjustments[sem_index] += undo_delta[sem_index];
  }

  slot->sem_otime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
  return 0;
}

intptr_t semget(key_t key, int nsems, int semflg) {
  if (nsems < 0 || nsems > MAX_SEMS_PER_SET)
    return -EINVAL;

  SemRegistry *reg = get_registry();
  if (!reg)
    return -ENOMEM;

  if (key != IPC_PRIVATE) {
    for (size_t i = 0; i < MAX_SEM_SETS; ++i) {
      SemSlot *slot = &reg->slots[i];
      if (slot->state.load(cpp::MemoryOrder::ACQUIRE) != SEM_SLOT_LIVE)
        continue;
      if (slot->key != key)
        continue;

      if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL))
        return -EEXIST;
      if (nsems > 0 && nsems > slot->nsems)
        return -EINVAL;
      if (!check_perms(slot, semflg))
        return -EACCES;
      return static_cast<intptr_t>(slot->semid);
    }

    if (!(semflg & IPC_CREAT))
      return -ENOENT;
  }

  if (nsems <= 0)
    return -EINVAL;

  SemSlot *slot = nullptr;
  size_t slot_index = 0;
  for (size_t i = 0; i < MAX_SEM_SETS; ++i) {
    uint32_t state = reg->slots[i].state.load(cpp::MemoryOrder::ACQUIRE);
    if (state == SEM_SLOT_EMPTY) {
      uint32_t expected = SEM_SLOT_EMPTY;
      if (reg->slots[i].state.compare_exchange_strong(
              expected, SEM_SLOT_CREATING, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED)) {
        slot = &reg->slots[i];
        slot_index = i;
        break;
      }
    } else if (state == SEM_SLOT_REMOVED) {
      uint32_t expected = SEM_SLOT_REMOVED;
      if (reg->slots[i].state.compare_exchange_strong(
              expected, SEM_SLOT_CREATING, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED)) {
        slot = &reg->slots[i];
        slot_index = i;
        break;
      }
    }
  }

  if (!slot)
    return -ENOSPC;

  int semid = reg->next_id.fetch_add(1, cpp::MemoryOrder::RELAXED);
  if (semid <= 0)
    semid = 1;

  HANDLE section = nullptr;
  SemSetHeader *shared = nullptr;
  if (!create_new_set(semid, nsems, &section, &shared)) {
    slot->state.store(SEM_SLOT_EMPTY, cpp::MemoryOrder::RELEASE);
    return -ENOMEM;
  }
  windows::ScopedNtHandle section_owned(section);

  windows::ScopedNtHandle lock_owned(create_or_open_named_mutant(semid));
  if (!lock_owned) {
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), shared, 0);
    slot->state.store(SEM_SLOT_EMPTY, cpp::MemoryOrder::RELEASE);
    return -ENOMEM;
  }

  lock_cache();
  size_t free_cache = MAX_SEM_CACHE;
  for (size_t i = 0; i < MAX_SEM_CACHE; ++i) {
    if (g_cached_sets[i].semid == -1) {
      free_cache = i;
      break;
    }
  }
  if (free_cache < MAX_SEM_CACHE) {
    g_cached_sets[free_cache].semid = semid;
    g_cached_sets[free_cache].nsems = nsems;
    g_cached_sets[free_cache].section = section_owned.release();
    g_cached_sets[free_cache].lock = lock_owned.release();
    g_cached_sets[free_cache].shared = shared;
  } else {
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), shared, 0);
    unlock_cache();
    slot->state.store(SEM_SLOT_EMPTY, cpp::MemoryOrder::RELEASE);
    return -EMFILE;
  }
  unlock_cache();

  pid_t pid = static_cast<pid_t>(NtCurrentProcessId());
  uid_t uid = windows_syscalls::geteuid();
  gid_t gid = windows_syscalls::getegid();
  mode_t mode = static_cast<mode_t>(semflg & 0777);

  slot->key = key;
  slot->semid = semid;
  slot->nsems = nsems;
  slot->sem_perm_mode = mode;
  slot->sem_perm_uid = uid;
  slot->sem_perm_gid = gid;
  slot->sem_perm_cuid = uid;
  slot->sem_perm_cgid = gid;
  slot->sem_perm_seq = static_cast<unsigned short>(slot_index);
  slot->sem_cpid = pid;
  slot->sem_otime.store(0, cpp::MemoryOrder::RELAXED);
  slot->sem_ctime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
  slot->state.store(SEM_SLOT_LIVE, cpp::MemoryOrder::RELEASE);
  return static_cast<intptr_t>(semid);
}

intptr_t semctl(int semid, int semnum, int cmd, intptr_t cmd_arg) {
  SemRegistry *reg = get_registry();
  if (!reg)
    return -ENOMEM;

  SemSlot *slot = find_slot_by_semid(reg, semid);
  if (!slot)
    return -EINVAL;
  if (slot->state.load(cpp::MemoryOrder::ACQUIRE) != SEM_SLOT_LIVE)
    return -EIDRM;

  CachedSemSet *cached = get_cached_set(semid, slot->nsems);
  if (!cached)
    return -EINVAL;

  if (!acquire_set_lock(cached->lock))
    return -EINTR;

  reap_dead_undo_slots(slot, cached->shared);
  SemValue *values = sem_values(cached->shared);
  pid_t pid = static_cast<pid_t>(NtCurrentProcessId());

  auto invalid_semnum = [&]() {
    return semnum < 0 || semnum >= cached->shared->nsems;
  };

  intptr_t result = 0;
  switch (cmd) {
  case IPC_RMID:
    slot->state.store(SEM_SLOT_REMOVED, cpp::MemoryOrder::RELEASE);
    result = 0;
    break;

  case IPC_SET: {
    auto *buf = reinterpret_cast<struct semid_ds *>(cmd_arg);
    if (!buf) {
      result = -EFAULT;
      break;
    }
    slot->sem_perm_uid = buf->sem_perm.uid;
    slot->sem_perm_gid = buf->sem_perm.gid;
    slot->sem_perm_mode = static_cast<mode_t>(buf->sem_perm.mode & 0777);
    slot->sem_ctime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
    result = 0;
    break;
  }

  case IPC_STAT: {
    auto *buf = reinterpret_cast<struct semid_ds *>(cmd_arg);
    if (!buf) {
      result = -EFAULT;
      break;
    }
    buf->sem_perm.__key = slot->key;
    buf->sem_perm.uid = slot->sem_perm_uid;
    buf->sem_perm.gid = slot->sem_perm_gid;
    buf->sem_perm.cuid = slot->sem_perm_cuid;
    buf->sem_perm.cgid = slot->sem_perm_cgid;
    buf->sem_perm.mode = slot->sem_perm_mode;
    buf->sem_perm.__seq = slot->sem_perm_seq;
    buf->sem_otime = slot->sem_otime.load(cpp::MemoryOrder::RELAXED);
    buf->sem_ctime = slot->sem_ctime.load(cpp::MemoryOrder::RELAXED);
    buf->sem_nsems = static_cast<decltype(buf->sem_nsems)>(slot->nsems);
    result = 0;
    break;
  }

  case GETVAL:
    if (invalid_semnum())
      result = -EINVAL;
    else
      result = values[semnum].value.load(cpp::MemoryOrder::RELAXED);
    break;

  case GETPID:
    if (invalid_semnum())
      result = -EINVAL;
    else
      result = values[semnum].last_pid.load(cpp::MemoryOrder::RELAXED);
    break;

  case GETNCNT:
    if (invalid_semnum())
      result = -EINVAL;
    else
      result =
          values[semnum].wait_negative.load(cpp::MemoryOrder::RELAXED);
    break;

  case GETZCNT:
    if (invalid_semnum())
      result = -EINVAL;
    else
      result = values[semnum].wait_zero.load(cpp::MemoryOrder::RELAXED);
    break;

  case SETVAL: {
    if (invalid_semnum()) {
      result = -EINVAL;
      break;
    }
    int value = static_cast<int>(cmd_arg);
    if (value < 0 || value > SEM_VALUE_MAXIMUM) {
      result = -ERANGE;
      break;
    }
    values[semnum].value.store(value, cpp::MemoryOrder::RELAXED);
    values[semnum].last_pid.store(pid, cpp::MemoryOrder::RELAXED);
    clear_undo_for_sem(cached->shared, semnum);
    slot->sem_ctime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
    result = 0;
    break;
  }

  case GETALL: {
    if (!cmd_arg) {
      result = -EFAULT;
      break;
    }
    auto *array = reinterpret_cast<unsigned short *>(cmd_arg);
    for (int i = 0; i < cached->shared->nsems; ++i)
      array[i] = static_cast<unsigned short>(
          values[i].value.load(cpp::MemoryOrder::RELAXED));
    result = 0;
    break;
  }

  case SETALL: {
    if (!cmd_arg) {
      result = -EFAULT;
      break;
    }
    auto *array = reinterpret_cast<unsigned short *>(cmd_arg);
    for (int i = 0; i < cached->shared->nsems; ++i) {
      if (array[i] > SEM_VALUE_MAXIMUM) {
        result = -ERANGE;
        break;
      }
    }
    if (result < 0)
      break;
    for (int i = 0; i < cached->shared->nsems; ++i) {
      values[i].value.store(array[i], cpp::MemoryOrder::RELAXED);
      values[i].last_pid.store(pid, cpp::MemoryOrder::RELAXED);
    }
    clear_all_undo(cached->shared);
    slot->sem_ctime.store(current_posix_time(), cpp::MemoryOrder::RELAXED);
    result = 0;
    break;
  }

  default:
    result = -EINVAL;
    break;
  }

  release_set_lock(cached->lock);
  return result;
}

intptr_t semop(int semid, struct sembuf *sops, size_t nsops) {
  if (!sops || nsops == 0 || nsops > static_cast<size_t>(MAX_SEMS_PER_SET))
    return -EINVAL;

  SemRegistry *reg = get_registry();
  if (!reg)
    return -ENOMEM;

  SemSlot *slot = find_slot_by_semid(reg, semid);
  if (!slot)
    return -EINVAL;
  if (slot->state.load(cpp::MemoryOrder::ACQUIRE) != SEM_SLOT_LIVE)
    return -EIDRM;

  CachedSemSet *cached = get_cached_set(semid, slot->nsems);
  if (!cached)
    return -EINVAL;

  pid_t pid = static_cast<pid_t>(NtCurrentProcessId());
  while (true) {
    if (!acquire_set_lock(cached->lock))
      return -EINTR;

    if (slot->state.load(cpp::MemoryOrder::ACQUIRE) != SEM_SLOT_LIVE) {
      release_set_lock(cached->lock);
      return -EIDRM;
    }

    reap_dead_undo_slots(slot, cached->shared);

    BlockedOp blocked;
    int ret = try_semop_locked(slot, cached->shared, sops, nsops, pid, &blocked);
    if (ret == 0) {
      release_set_lock(cached->lock);
      return 0;
    }

    bool should_wait = (ret == -EAGAIN) && (blocked.sem_index >= 0);
    if (!should_wait) {
      release_set_lock(cached->lock);
      return ret;
    }

    SemValue *values = sem_values(cached->shared);
    if (blocked.wait_zero)
      values[blocked.sem_index].wait_zero.fetch_add(1, cpp::MemoryOrder::RELAXED);
    else
      values[blocked.sem_index].wait_negative.fetch_add(
          1, cpp::MemoryOrder::RELAXED);
    release_set_lock(cached->lock);

    sleep_for_100ns(SEM_WAIT_INTERVAL_100NS);

    if (!acquire_set_lock(cached->lock))
      return -EINTR;
    if (blocked.wait_zero)
      values[blocked.sem_index].wait_zero.fetch_sub(1, cpp::MemoryOrder::RELAXED);
    else
      values[blocked.sem_index].wait_negative.fetch_sub(
          1, cpp::MemoryOrder::RELAXED);
    bool removed =
        slot->state.load(cpp::MemoryOrder::ACQUIRE) != SEM_SLOT_LIVE;
    release_set_lock(cached->lock);
    if (removed)
      return -EIDRM;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
