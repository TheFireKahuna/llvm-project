//===--- Definitions of common thread items ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/thread.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#include "src/__support/CPP/array.h"
#include "src/__support/CPP/mutex.h" // lock_guard
#include "src/__support/CPP/optional.h"
#include "src/__support/macros/attributes.h"

#ifdef __NTPOSIX__
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#endif

namespace LIBC_NAMESPACE_DECL {
namespace {

using AtExitCallback = void(void *);

// All fields zero-initialized by thread_local (inline block) or page_alloc
// (overflow blocks). No default member initializers — keeps the enclosing
// ThreadAtExitCallbackMgr trivially constructible so thread_local doesn't
// need per-thread constructor code (-Wglobal-constructors).
struct AtExitUnit {
  AtExitCallback *callback;
  void *obj;
  void *dso;
};

// Block-based demand-allocated list for thread_local destructor registrations.
// The first block is inline in TLS (zero allocation). Overflow blocks use
// page_alloc on NTPOSIX (zero init dependency — direct NtAllocateVirtualMemory).
constexpr size_t kAtExitEntriesPerBlock = 30;

struct AtExitBlock {
  AtExitUnit entries[kAtExitEntriesPerBlock];
  AtExitBlock *next;
  size_t count;
};

#ifdef __NTPOSIX__
AtExitBlock *alloc_atexit_block() {
  void *p = internal::page_alloc(sizeof(AtExitBlock));
  if (!p)
    return nullptr;
  // page_alloc returns zero-initialized memory; set fields explicitly
  // for clarity.
  auto *block = static_cast<AtExitBlock *>(p);
  block->next = nullptr;
  block->count = 0;
  return block;
}
void free_atexit_block(AtExitBlock *block) {
  if (block)
    internal::page_free(block);
}
#else
AtExitBlock *alloc_atexit_block() { return nullptr; }
void free_atexit_block(AtExitBlock *) {}
#endif

// ---------------------------------------------------------------------------
// Unloaded DSO registry — process-wide, lock-free reads.
//
// When a shared library is unloaded (dlclose / FreeLibrary), its __dso_handle
// is recorded here. During thread exit, destructors whose DSO matches an
// unloaded entry are skipped to prevent calling into unmapped code.
//
// Writers (record_dso_unload) are serialized by unloaded_dso_write_mtx.
// Readers (is_dso_unloaded) use acquire/release ordering on the count —
// no mutex needed on the read path.
// ---------------------------------------------------------------------------
constexpr size_t kMaxUnloadedDsos = 64;
void *unloaded_dso_storage[kMaxUnloadedDsos];
cpp::Atomic<size_t> unloaded_dso_count{0};
Mutex unloaded_dso_write_mtx(/*timed=*/false, /*recursive=*/false,
                             /*robust=*/false, /*pshared=*/false);

bool is_dso_unloaded(void *dso) {
  if (!dso)
    return false;
  size_t count = unloaded_dso_count.load(cpp::MemoryOrder::ACQUIRE);
  for (size_t i = 0; i < count; ++i) {
    if (unloaded_dso_storage[i] == dso)
      return true;
  }
  return false;
}

void record_dso_unload(void *dso) {
  if (!dso)
    return;
  cpp::lock_guard lock(unloaded_dso_write_mtx);
  size_t count = unloaded_dso_count.load(cpp::MemoryOrder::RELAXED);
  for (size_t i = 0; i < count; ++i) {
    if (unloaded_dso_storage[i] == dso)
      return;
  }
  if (count >= kMaxUnloadedDsos)
    return;
  unloaded_dso_storage[count] = dso;
  unloaded_dso_count.store(count + 1, cpp::MemoryOrder::RELEASE);
}

constexpr size_t TSS_KEY_COUNT = 1024;

struct TSSKeyUnit {
  // Indicates whether is unit is active. Presence of a non-null dtor
  // is not sufficient to indicate the same information as a TSS key can
  // have a null destructor.
  bool active = false;

  TSSDtor *dtor = nullptr;

  constexpr TSSKeyUnit() = default;
  constexpr TSSKeyUnit(TSSDtor *d) : active(true), dtor(d) {}

  void reset() {
    active = false;
    dtor = nullptr;
  }
};

class TSSKeyMgr {
  Mutex mtx;
  cpp::array<TSSKeyUnit, TSS_KEY_COUNT> units;

public:
  constexpr TSSKeyMgr()
      : mtx(/*timed=*/false, /*recursive=*/false, /*robust=*/false,
            /*pshared=*/false) {}

  cpp::optional<unsigned int> new_key(TSSDtor *dtor) {
    cpp::lock_guard lock(mtx);
    for (unsigned int i = 0; i < TSS_KEY_COUNT; ++i) {
      TSSKeyUnit &u = units[i];
      if (!u.active) {
        u = {dtor};
        return i;
      }
    }
    return cpp::optional<unsigned int>();
  }

  TSSDtor *get_dtor(unsigned int key) {
    if (key >= TSS_KEY_COUNT)
      return nullptr;
    cpp::lock_guard lock(mtx);
    return units[key].dtor;
  }

  bool remove_key(unsigned int key) {
    if (key >= TSS_KEY_COUNT)
      return false;
    cpp::lock_guard lock(mtx);
    units[key].reset();
    return true;
  }

  bool is_valid_key(unsigned int key) {
    cpp::lock_guard lock(mtx);
    return units[key].active;
  }
};

TSSKeyMgr tss_key_mgr;

struct TSSValueUnit {
  bool active = false;
  void *payload = nullptr;
  TSSDtor *dtor = nullptr;

  constexpr TSSValueUnit() = default;
  constexpr TSSValueUnit(void *p, TSSDtor *d)
      : active(true), payload(p), dtor(d) {}
};

static LIBC_THREAD_LOCAL cpp::array<TSSValueUnit, TSS_KEY_COUNT> tss_values;

} // anonymous namespace

// Trivially constructible — zero-init by thread_local is the valid initial
// state (lock_word=0 unlocked, current_block=nullptr lazy-init, first_block
// all zeros = empty block). No per-thread constructor code needed.
//
// Uses a bare atomic spinlock instead of Mutex to stay trivially
// constructible. The lock is per-thread (thread_local) so it's never truly
// contended across threads — it only serializes re-entrant add_callback
// calls from within destructor callbacks invoked by call().
class ThreadAtExitCallbackMgr {
  cpp::Atomic<uint32_t> lock_word;
  AtExitBlock first_block;
  AtExitBlock *current_block;

  void acquire() {
    while (lock_word.exchange(1, cpp::MemoryOrder::ACQUIRE) != 0)
      ;
  }
  void release() { lock_word.store(0, cpp::MemoryOrder::RELEASE); }

public:
  int add_callback(AtExitCallback *callback, void *obj, void *dso) {
    acquire();
    if (!current_block)
      current_block = &first_block;
    if (current_block->count >= kAtExitEntriesPerBlock) {
      AtExitBlock *overflow = alloc_atexit_block();
      if (!overflow) {
        release();
        return -1;
      }
      overflow->next = current_block;
      current_block = overflow;
    }
    auto &entry = current_block->entries[current_block->count++];
    entry.callback = callback;
    entry.obj = obj;
    entry.dso = dso;
    release();
    return 0;
  }

  void call(void *dso) {
    acquire();
    if (!dso) {
      // Full cleanup — LIFO order, handles re-registration during callbacks.
      for (;;) {
        if (!current_block)
          break;
        if (current_block->count == 0) {
          if (current_block == &first_block) {
            current_block = nullptr;
            break;
          }
          AtExitBlock *empty = current_block;
          current_block = empty->next;
          free_atexit_block(empty);
          continue;
        }
        auto &entry = current_block->entries[--current_block->count];
        if (!entry.callback)
          continue;
        if (entry.dso && is_dso_unloaded(entry.dso)) {
          entry.callback = nullptr;
          continue;
        }
        auto cb = entry.callback;
        auto obj_ptr = entry.obj;
        entry.callback = nullptr;
        release();
        cb(obj_ptr);
        acquire();
      }
      first_block.next = nullptr;
    } else {
      // DSO-specific cleanup — run only destructors for this DSO.
      AtExitBlock *block = current_block;
      while (block) {
        for (size_t i = block->count; i > 0;) {
          --i;
          auto &entry = block->entries[i];
          if (!entry.callback || entry.dso != dso)
            continue;
          auto cb = entry.callback;
          auto obj_ptr = entry.obj;
          entry.callback = nullptr;
          release();
          cb(obj_ptr);
          acquire();
        }
        block = block->next;
      }
    }
    release();
  }
};

static LIBC_THREAD_LOCAL ThreadAtExitCallbackMgr atexit_callback_mgr;

// The function __cxa_thread_atexit is provided by C++ runtimes like libcxxabi.
// It is used by thread local object runtime to register destructor calls. To
// actually register destructor call with the threading library, it calls
// __cxa_thread_atexit_impl, which is to be provided by the threading library.
// The semantics are very similar to the __cxa_atexit function except for the
// fact that the registered callback is thread specific.
extern "C" int __cxa_thread_atexit_impl(AtExitCallback *callback, void *obj,
                                        void *dso) {
  return atexit_callback_mgr.add_callback(callback, obj, dso);
}

namespace internal {

ThreadAtExitCallbackMgr *get_thread_atexit_callback_mgr() {
  return &atexit_callback_mgr;
}

void call_atexit_callbacks(ThreadAttributes *attrib, void *dso) {
  // attrib may be nullptr for the main thread (it never goes through
  // thread_entry_impl so self.attrib is not set). Skip the per-thread
  // atexit callbacks but still run TSS destructors below.
  if (attrib && attrib->atexit_callback_mgr)
    attrib->atexit_callback_mgr->call(dso);
  // TSS destructors only run on full thread cleanup, not DSO-specific.
  if (!dso) {
    // POSIX requires up to PTHREAD_DESTRUCTOR_ITERATIONS rounds of TSS
    // destructor calls. A destructor may call pthread_setspecific, re-arming
    // a slot for the next round.
    constexpr int TSS_DTOR_ITERATIONS = 4;
    for (int round = 0; round < TSS_DTOR_ITERATIONS; ++round) {
      bool any_called = false;
      for (size_t i = 0; i < TSS_KEY_COUNT; ++i) {
        TSSValueUnit &unit = tss_values[i];
        if (unit.dtor != nullptr && unit.payload != nullptr) {
          void *val = unit.payload;
          unit.payload = nullptr; // Clear before invoke (POSIX).
          any_called = true;
          unit.dtor(val);
        }
      }
      if (!any_called)
        break;
    }
  }
}

extern "C" __attribute__((visibility("default"))) __declspec(dllexport)
void __cxa_thread_finalize(void *dso) {
  call_atexit_callbacks(self.attrib, dso);
}

extern "C" __attribute__((visibility("default"))) __declspec(dllexport)
void __cxa_thread_finalize_dso_unload(void *dso) {
  if (!dso)
    return;
  call_atexit_callbacks(self.attrib, dso);
  record_dso_unload(dso);
}

} // namespace internal

cpp::optional<unsigned int> new_tss_key(TSSDtor *dtor) {
  return tss_key_mgr.new_key(dtor);
}

bool tss_key_delete(unsigned int key) { return tss_key_mgr.remove_key(key); }

bool set_tss_value(unsigned int key, void *val) {
  if (!tss_key_mgr.is_valid_key(key))
    return false;
  tss_values[key] = {val, tss_key_mgr.get_dtor(key)};
  return true;
}

void *get_tss_value(unsigned int key) {
  if (key >= TSS_KEY_COUNT)
    return nullptr;

  auto &u = tss_values[key];
  if (!u.active)
    return nullptr;
  return u.payload;
}

} // namespace LIBC_NAMESPACE_DECL
