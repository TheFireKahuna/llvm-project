//===-- dlfcn engine: dlopen/dlsym/dlclose/dladdr/dlinfo ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Business logic for POSIX dlfcn on Windows NT. All functions return 0/-errno
// (long) or value/-errno (intptr_t). No libc_errno, no dlfcn_state.
//
//===----------------------------------------------------------------------===//

#include "dlfcn_ops.h"

#include "hdr/dlfcn_macros.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_reconcile.h"
#include "src/__support/OSUtil/windows/nt/nt_path_convert.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/OSUtil/windows/nt/nt_string.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

// Bare DLL names (no slash) use the NT loader's default search path and
// never exceed MAX_PATH. Paths containing a slash go through to_nt_path()
// with its own larger buffer — this constant does not bound them.
inline constexpr int DLOPEN_BARE_NAME_MAX = 261;

namespace LIBC_NAMESPACE_DECL {

namespace {

// Walk the PE export table to find the nearest named export <= addr.
void find_nearest_export(PVOID base, const void *addr, Dl_info *info) {
  info->dli_sname = nullptr;
  info->dli_saddr = nullptr;

  auto *nt_hdr = static_cast<IMAGE_NT_HEADERS64 *>(::RtlImageNtHeader(base));
  if (!nt_hdr)
    return;

  auto &exp_entry =
      nt_hdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!exp_entry.VirtualAddress || !exp_entry.Size)
    return;

  auto base_addr = reinterpret_cast<ULONG_PTR>(base);
  auto *exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(
      base_addr + exp_entry.VirtualAddress);

  auto *functions =
      reinterpret_cast<ULONG *>(base_addr + exports->AddressOfFunctions);
  auto *names =
      reinterpret_cast<ULONG *>(base_addr + exports->AddressOfNames);
  auto *ordinals =
      reinterpret_cast<USHORT *>(base_addr + exports->AddressOfNameOrdinals);

  ULONG_PTR target = reinterpret_cast<ULONG_PTR>(addr);
  ULONG_PTR best_addr = 0;
  ULONG best_name_rva = 0;

  // Scan named exports for the closest address <= target.
  for (ULONG i = 0; i < exports->NumberOfNames; ++i) {
    USHORT ordinal = ordinals[i];
    if (ordinal >= exports->NumberOfFunctions)
      continue;
    ULONG_PTR fn_addr = base_addr + functions[ordinal];
    // Skip forwarded exports (RVA falls within the export directory).
    if (fn_addr >= reinterpret_cast<ULONG_PTR>(exports) &&
        fn_addr < reinterpret_cast<ULONG_PTR>(exports) + exp_entry.Size)
      continue;
    if (fn_addr <= target && fn_addr > best_addr) {
      best_addr = fn_addr;
      best_name_rva = names[i];
    }
  }

  if (best_addr) {
    info->dli_sname = reinterpret_cast<const char *>(base_addr + best_name_rva);
    info->dli_saddr = reinterpret_cast<void *>(best_addr);
  }
}

} // anonymous namespace

// Sentinel address returned by dlopen(NULL). POSIX requires this handle to
// represent the "global symbol scope" — dlsym on it must search all modules
// loaded with RTLD_GLOBAL, not just the main executable. We use a private
// static so the pointer is unique and cannot collide with any real module base.
static const char global_scope_sentinel = 0;

// ---------------------------------------------------------------------------
// RTLD_GLOBAL module set.
//
// Tracks module base addresses opened with RTLD_GLOBAL. The main executable
// is always implicitly global and not stored here.
//
// Backing store: demand-commit array via page_reserve/page_commit. Pages are
// decommitted when the set shrinks, and the reservation is released when the
// set becomes empty. Swap-remove keeps the array dense.
//
// Concurrency: seqlock (read-copy pattern). Writers hold a spinlock for
// mutual exclusion and bracket mutations with seq increments. Readers load
// the sequence before and after scanning — if it changed or is odd (write
// in progress), they retry. This makes the dlsym read path completely
// lock-free when no concurrent dlopen/dlclose is happening.
//
// fork_reinit: reset the spinlock and sequence counter. The array contents
// remain valid — module bases are inherited across fork.
// ---------------------------------------------------------------------------

namespace {
using Atomic_uint = LIBC_NAMESPACE::cpp::Atomic<unsigned>;
using Atomic_int = LIBC_NAMESPACE::cpp::Atomic<int>;
using MemOrd = LIBC_NAMESPACE::cpp::MemoryOrder;

struct GlobalModuleSet {
  static constexpr size_t RESERVE_SIZE = 65536; // 64 KB VA
  static constexpr size_t PAGE = 4096;

  PVOID *entries = nullptr;
  int count = 0;
  int committed = 0;     // capacity in slots backed by committed pages

  Atomic_uint seq{0};    // seqlock: even = stable, odd = write in progress
  Futex wlock{0};        // writer mutual exclusion

  void writer_lock() {
    for (;;) {
      FutexValueType expected = 0;
      if (wlock.compare_exchange_weak(expected, 1, MemOrd::ACQUIRE,
                                      MemOrd::RELAXED))
        return;
      // Yield on transient wait failure (-ENOMEM from wait-slot pool
      // exhaustion). The dlfcn writer lock has no caller-reachable
      // error path, so yielding is the only way to avoid a hot spin.
      long ret = wlock.wait(1);
      if (ret < 0 && ret != -EINTR)
        ::NtYieldExecution();
    }
  }

  void writer_unlock() { wlock.store_and_notify(0); }

  // -- Seqlock brackets (caller holds writer lock) --

  void write_begin() { seq.fetch_add(1, MemOrd::RELEASE); }
  void write_end() { seq.fetch_add(1, MemOrd::RELEASE); }

  // -- Capacity management (caller holds writer lock) --

  bool ensure_capacity(int needed) {
    if (needed <= committed)
      return true;
    if (!entries) {
      entries = static_cast<PVOID *>(
          LIBC_NAMESPACE::internal::page_reserve(RESERVE_SIZE));
      if (!entries)
        return false;
    }
    size_t bytes = static_cast<size_t>(needed) * sizeof(PVOID);
    bytes = (bytes + PAGE - 1) & ~(PAGE - 1);
    if (bytes > RESERVE_SIZE)
      return false;
    if (!LIBC_NAMESPACE::internal::page_commit(entries, bytes))
      return false;
    committed = static_cast<int>(bytes / sizeof(PVOID));
    return true;
  }

  void shrink() {
    if (count == 0 && entries) {
      LIBC_NAMESPACE::internal::page_free(entries);
      entries = nullptr;
      committed = 0;
      return;
    }
    size_t used = (static_cast<size_t>(count) * sizeof(PVOID) + PAGE - 1)
                  & ~(PAGE - 1);
    size_t have = static_cast<size_t>(committed) * sizeof(PVOID);
    if (have > used) {
      LIBC_NAMESPACE::internal::page_decommit(
          reinterpret_cast<char *>(entries) + used, have - used);
      committed = static_cast<int>(used / sizeof(PVOID));
    }
  }

  // -- Public operations --

  void add(PVOID base) {
    writer_lock();
    // Check for duplicate before entering the write-side seqlock bracket
    // so readers aren't stalled by a no-op.
    for (int i = 0; i < count; ++i) {
      if (entries[i] == base) {
        writer_unlock();
        return;
      }
    }
    if (ensure_capacity(count + 1)) {
      write_begin();
      entries[count++] = base;
      write_end();
    }
    writer_unlock();
  }

  void remove(PVOID base) {
    writer_lock();
    for (int i = 0; i < count; ++i) {
      if (entries[i] == base) {
        write_begin();
        entries[i] = entries[--count];
        write_end();
        shrink();
        break;
      }
    }
    writer_unlock();
  }

  bool contains(PVOID base) {
    // Fast path: exe is always global.
    if (base == NtCurrentPeb()->ImageBaseAddress)
      return true;
    // Seqlock read: retry if a concurrent write is detected.
    for (;;) {
      unsigned s1 = seq.load(MemOrd::ACQUIRE);
      if (s1 & 1u) {
        // Write in progress — park the CPU in UMWAIT / MWAITX on the
        // seqlock word until the writer's even-parity store wakes us.
        spin_wait::spin_on_raw(
            reinterpret_cast<unsigned *>(&seq.val), s1, 1024);
        continue;
      }
      bool found = false;
      int n = count;
      for (int i = 0; i < n; ++i) {
        if (entries[i] == base) {
          found = true;
          break;
        }
      }
      unsigned s2 = seq.load(MemOrd::ACQUIRE);
      if (s1 == s2)
        return found;
      // Sequence changed — a write happened during our scan, retry.
    }
  }

  // Reset lock state after fork. Array contents are still valid.
  void fork_reinit() {
    wlock.reset_for_fork(0);
    // Sequence is even (no write in progress) — snap to current value
    // rounded up to even.
    unsigned s = seq.load(MemOrd::RELAXED);
    if (s & 1u)
      seq.store(s + 1, MemOrd::RELAXED);
  }
};

} // anonymous namespace

static GlobalModuleSet global_modules;

namespace internal {

// ---------------------------------------------------------------------------
// dlopen — POSIX dlopen via LdrLoadDll (NT loader).
// ---------------------------------------------------------------------------
intptr_t dlopen(const char *path, int mode) {
  // dlopen(NULL, ...) returns a global-scope handle per POSIX.
  // dlsym on this handle searches all loaded modules in load order.
  if (!path)
    return reinterpret_cast<intptr_t>(&global_scope_sentinel);

  using LIBC_NAMESPACE::cpp::string_view;
  string_view sv(path);
  if (sv.empty())
    return -EINVAL;

  // POSIX convention: a path with any slash is a filesystem path (absolute
  // or relative to CWD); a bare name goes through the default search path.
  // The NT loader's LdrpSearchPath does not honour CWD and does not accept
  // forward slashes, so paths with a slash must be resolved to an absolute
  // native path before handing them off. Bare names are passed verbatim so
  // LdrLoadDll can apply its standard DLL search order.
  bool is_filesystem_path = sv.contains('/') || sv.contains('\\');

  // Resolve into a scratch buffer. Both branches write a native wide string
  // into `wpath` with length `wlen` (in WCHARs, excluding the NUL).
  auto path_s = internal::path_scratch();
  if (!path_s)
    return -ENOMEM;
  WCHAR *wpath = path_s.data();
  size_t wlen = 0;

  if (is_filesystem_path) {
    auto nt = to_nt_path(sv, wpath, path_s.size());
    if (!nt.has_value())
      return -nt.error();
    wlen = nt.value();
  } else {
    if (sv.size() >= DLOPEN_BARE_NAME_MAX)
      return -ENAMETOOLONG;
    int w = windows::utf8_to_wide(sv, wpath, DLOPEN_BARE_NAME_MAX);
    if (w <= 0)
      return -EINVAL;
    wlen = static_cast<size_t>(w - 1);
  }

  windows::nt_wstring_view dll_wsv(wpath, wlen);

  // RTLD_NOLOAD: check if already loaded without incrementing the ref count.
  if (mode & RTLD_NOLOAD) {
    PVOID handle = nullptr;
    NTSTATUS st = ::LdrGetDllHandleByName(dll_wsv.unicode_string(), nullptr, &handle);
    if (NT_ERROR(st))
      return windows_util::ntstatus_to_kerr(st);
    return reinterpret_cast<intptr_t>(handle);
  }

  // Normal load via NT loader.
  PVOID handle = nullptr;
  NTSTATUS st = ::LdrLoadDll(nullptr, nullptr, dll_wsv.unicode_string(), &handle);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);

  // Track modules opened with RTLD_GLOBAL for global-scope symbol search.
  if (mode & RTLD_GLOBAL)
    global_modules.add(handle);

  // Bound-the-window reconcile: the loader may have stamped placeholder VA
  // (and dependent-DLL VA) inside ranges our hint logic cares about.
  // Walk the table over [DllBase, DllBase + SizeOfImage) and cordon any
  // foreign placeholders that landed there as FOREIGN. Locate the LDR
  // entry by base address; if missing for any reason, fall back to a
  // generous fixed window so dependent DLLs immediately past the primary
  // are still covered.
  {
    SIZE_T module_size = 0;
    PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
    if (ldr) {
      LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
      for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
        auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
        if (entry->DllBase == handle) {
          module_size = entry->SizeOfImage;
          break;
        }
      }
    }
    if (module_size == 0)
      module_size = 256ULL << 20; // 256 MiB fallback window.
    (void)windows::memory::post_dlopen_reconcile(handle, module_size);
  }

  return reinterpret_cast<intptr_t>(handle);
}

// ---------------------------------------------------------------------------
// dlsym — POSIX dlsym via LdrGetProcedureAddress (NT loader).
// ---------------------------------------------------------------------------
intptr_t dlsym(void *__restrict handle, const char *__restrict symbol) {
  if (handle == RTLD_NEXT)
    return -ENOSYS;

  ANSI_STRING proc_name;
  ::RtlInitString(&proc_name, symbol);

  // RTLD_DEFAULT or dlopen(NULL) handle: search modules in load order,
  // but only those in the global scope (exe + RTLD_GLOBAL modules).
  if (!handle || handle == &global_scope_sentinel) {
    PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
      auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
      if (!global_modules.contains(entry->DllBase))
        continue;
      PVOID addr = nullptr;
      NTSTATUS st =
          ::LdrGetProcedureAddress(entry->DllBase, &proc_name, 0, &addr);
      if (NT_SUCCESS(st) && addr)
        return reinterpret_cast<intptr_t>(addr);
    }
    return -ENOENT;
  }

  // Explicit handle: resolve from that module only.
  PVOID addr = nullptr;
  NTSTATUS st = ::LdrGetProcedureAddress(handle, &proc_name, 0, &addr);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  return reinterpret_cast<intptr_t>(addr);
}

// ---------------------------------------------------------------------------
// dlclose — POSIX dlclose via LdrUnloadDll.
// ---------------------------------------------------------------------------
intptr_t dlclose(void *handle) {
  if (!handle)
    return -EINVAL;

  // Closing the global scope handle from dlopen(NULL) is a no-op.
  if (handle == &global_scope_sentinel)
    return 0;

  // Remove from global set before unloading (no-op if not present).
  global_modules.remove(handle);

  NTSTATUS st = ::LdrUnloadDll(handle);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  return 0;
}

// ---------------------------------------------------------------------------
// dladdr — POSIX dladdr via RtlPcToFileHeader + PE export table walk.
// Returns 1 on success, 0 on failure (no errno per POSIX).
// ---------------------------------------------------------------------------

// Thread-local buffer for dli_fname. POSIX permits invalidation on next call.
static thread_local char fname_buf[MAX_PATH * 3 + 1];

intptr_t dladdr(const void *__restrict addr, Dl_info *__restrict info) {
  if (!addr || !info)
    return 0;

  // Find the module containing this address.
  PVOID base = nullptr;
  if (!::RtlPcToFileHeader(const_cast<PVOID>(addr), &base) || !base)
    return 0;

  info->dli_fbase = base;
  info->dli_fname = nullptr;

  // Walk PEB loader list to find the module path.
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
    // InLoadOrderLinks is at offset 0 — cast directly.
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    if (entry->DllBase == base) {
      int len = windows::wide_to_utf8_n(
          entry->FullDllName.Buffer,
          entry->FullDllName.Length / sizeof(WCHAR),
          fname_buf, sizeof(fname_buf) - 1);
      if (len > 0) {
        fname_buf[len] = '\0';
        info->dli_fname = fname_buf;
      }
      break;
    }
  }

  // Walk PE export table for nearest symbol.
  find_nearest_export(base, addr, info);

  return 1;
}

// ---------------------------------------------------------------------------
// dlinfo — GNU dlinfo extension. Only RTLD_DI_ORIGIN supported.
// ---------------------------------------------------------------------------
intptr_t dlinfo(void *__restrict handle, int request, void *__restrict info) {
  if (!handle)
    return -EINVAL;

  if (request == RTLD_DI_ORIGIN) {
    // Return the directory containing the shared object.
    // Walk PEB loader list to find the module path, then strip the filename.
    PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
      auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
      if (entry->DllBase == handle) {
        // Convert wide path to UTF-8.
        auto *dst = static_cast<char *>(info);
        int len = windows::wide_to_utf8_n(
            entry->FullDllName.Buffer,
            entry->FullDllName.Length / sizeof(WCHAR), dst, MAX_PATH * 3);
        if (len <= 0)
          return -EIO;
        dst[len] = '\0';
        // Strip filename — find last separator.
        int last_sep = -1;
        for (int i = 0; i < len; ++i) {
          if (dst[i] == '\\' || dst[i] == '/')
            last_sep = i;
        }
        if (last_sep >= 0)
          dst[last_sep] = '\0';
        return 0;
      }
    }
    return -ESRCH;
  }

  // All other request codes are ELF-specific or unsupported.
  return -ENOSYS;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

// global_modules is a file-scope static in the LIBC_NAMESPACE_DECL namespace
// above. Its type (GlobalModuleSet) is in an anonymous namespace within that
// block, but the variable itself is accessible here via its qualified name.
void LIBC_NAMESPACE::internal::dlfcn_fork_reinit() {
  LIBC_NAMESPACE::global_modules.fork_reinit();
}

LIBC_REGISTER_FORK_REINIT(dlfcn,
                          ::LIBC_NAMESPACE::internal::kForkPrioDlfcn,
                          &::LIBC_NAMESPACE::internal::dlfcn_fork_reinit)
