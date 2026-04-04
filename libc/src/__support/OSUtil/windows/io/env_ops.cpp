//===-- Internal environment variable operations --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/io/env_ops.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"
#include "src/unistd/environ.h"

#include "hdr/errno_macros.h"
#include "src/__support/libc_errno.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Ownership bitmap helpers
//
// The bitmap is packed after the char*[capacity] array in a single malloc
// block: [ char*[capacity] | uint8_t[(capacity+7)/8] ]
// =========================================================================

static inline uint8_t *env_bitmap() {
  return reinterpret_cast<uint8_t *>(g_pcb.environment.ptrs +
                                     g_pcb.environment.capacity);
}

static inline bool env_is_owned(int idx) {
  return (env_bitmap()[idx / 8] >> (idx % 8)) & 1;
}

static inline void env_set_owned(int idx, bool owned) {
  uint8_t &byte = env_bitmap()[idx / 8];
  uint8_t mask = static_cast<uint8_t>(1u << (idx % 8));
  if (owned)
    byte |= mask;
  else
    byte &= static_cast<uint8_t>(~mask);
}

// =========================================================================
// Internal helpers (caller holds env_lock)
// =========================================================================

static constexpr char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

static bool env_name_equal_ignore_case(cpp::string_view lhs,
                                       cpp::string_view rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower(lhs[i]) != ascii_lower(rhs[i]))
      return false;
  }
  return true;
}

// Find index of name in env_ptrs, or -1.
static int env_find(cpp::string_view name) {
  if (!g_pcb.environment.ptrs)
    return -1;
  for (int i = 0; i < g_pcb.environment.count; ++i) {
    cpp::string_view entry(g_pcb.environment.ptrs[i]);
    if (entry.size() > name.size() && entry[name.size()] == '=' &&
        env_name_equal_ignore_case(
            entry.substr(0, name.size()), name))
      return i;
  }
  return -1;
}

// Validate a name: must be non-null, non-empty, and contain no '='.
static bool env_name_valid(const char *name) {
  if (!name || name[0] == '\0')
    return false;
  return !cpp::string_view(name).contains('=');
}


// Allocate a "NAME=VALUE" string via malloc.
static char *env_make_entry(cpp::string_view name, cpp::string_view value) {
  // name.size() + 1 ('=') + value.size() + 1 ('\0')
  size_t total = name.size() + 1 + value.size() + 1;
  auto *entry = static_cast<char *>(LIBC_NAMESPACE::malloc(total));
  if (!entry)
    return nullptr;
  __builtin_memcpy(entry, name.data(), name.size());
  entry[name.size()] = '=';
  __builtin_memcpy(entry + name.size() + 1, value.data(), value.size());
  entry[total - 1] = '\0';
  return entry;
}

// Detect if application reassigned the `environ` global and re-import.
// Caller holds env_lock.
static void env_check_divergence() {
  if (LIBC_NAMESPACE::environ == g_pcb.environment.ptrs)
    return;
  if (!LIBC_NAMESPACE::environ) {
    // Application set environ = NULL — treat as empty environment.
    // Don't free old entries — we can't know if they're still referenced.
    g_pcb.environment.ptrs = nullptr;
    g_pcb.environment.count = 0;
    g_pcb.environment.capacity = 0;
    return;
  }

  // Application replaced environ with a foreign array. Count entries.
  int count = 0;
  while (LIBC_NAMESPACE::environ[count])
    ++count;

  int capacity = count + 1;
  size_t ptrs_bytes = static_cast<size_t>(capacity) * sizeof(char *);
  size_t bitmap_bytes = (static_cast<size_t>(capacity) + 7) / 8;
  auto *block = static_cast<char **>(LIBC_NAMESPACE::malloc(ptrs_bytes + bitmap_bytes));
  if (!block) {
    // Allocation failed — keep using the foreign pointer directly.
    // This is best-effort. The old PCB block leaks, but we can't free it
    // safely (old owned strings might still be referenced).
    g_pcb.environment.ptrs = LIBC_NAMESPACE::environ;
    g_pcb.environment.count = count;
    g_pcb.environment.capacity = capacity;
    return;
  }

  __builtin_memcpy(block, LIBC_NAMESPACE::environ,
                   static_cast<size_t>(capacity) * sizeof(char *));
  auto *bitmap = reinterpret_cast<uint8_t *>(block + capacity);
  __builtin_memset(bitmap, 0, bitmap_bytes); // All unowned — foreign strings.

  // Note: we don't free the old env_ptrs block here. The old strings might
  // still be referenced by the application (e.g., saved getenv pointers).
  // This is a deliberate leak on environ reassignment, matching glibc.
  g_pcb.environment.ptrs = block;
  g_pcb.environment.count = count;
  g_pcb.environment.capacity = capacity;
  LIBC_NAMESPACE::environ = block;
}

// Grow the env_ptrs+bitmap block to at least new_capacity.
// Caller holds env_lock. Returns true on success.
static bool env_grow(int new_capacity) {
  size_t ptrs_bytes = static_cast<size_t>(new_capacity) * sizeof(char *);
  size_t bitmap_bytes = (static_cast<size_t>(new_capacity) + 7) / 8;
  auto *block = static_cast<char **>(LIBC_NAMESPACE::malloc(ptrs_bytes + bitmap_bytes));
  if (!block)
    return false;

  // Copy existing pointers (count + null terminator).
  if (g_pcb.environment.ptrs && g_pcb.environment.count > 0) {
    __builtin_memcpy(
        block, g_pcb.environment.ptrs,
        static_cast<size_t>(g_pcb.environment.count + 1) * sizeof(char *));
  }
  // Zero new pointer slots.
  for (int i = g_pcb.environment.count + 1; i < new_capacity; ++i)
    block[i] = nullptr;

  // Copy existing bitmap.
  auto *new_bitmap = reinterpret_cast<uint8_t *>(block + new_capacity);
  size_t old_bitmap_bytes =
      (static_cast<size_t>(g_pcb.environment.capacity) + 7) / 8;
  __builtin_memset(new_bitmap, 0, bitmap_bytes);
  if (g_pcb.environment.ptrs && old_bitmap_bytes > 0)
    __builtin_memcpy(new_bitmap, env_bitmap(),
                     old_bitmap_bytes < bitmap_bytes ? old_bitmap_bytes
                                                    : bitmap_bytes);

  // Free old block.
  if (g_pcb.environment.ptrs)
    LIBC_NAMESPACE::free(g_pcb.environment.ptrs);

  g_pcb.environment.ptrs = block;
  g_pcb.environment.capacity = new_capacity;
  return true;
}

// Sync the environ export after a mutation.
static inline void env_sync() {
  LIBC_NAMESPACE::environ = g_pcb.environment.ptrs;
}

// =========================================================================
// Public internal API
// =========================================================================

char *env_get(const char *name) {
  if (!name || name[0] == '\0')
    return nullptr;

  cpp::string_view name_sv(name);

  g_pcb.environment.lock.lock();
  env_check_divergence();

  int idx = env_find(name_sv);
  char *result = nullptr;
  if (idx >= 0)
    result = g_pcb.environment.ptrs[idx] + name_sv.size() + 1; // skip "NAME="

  g_pcb.environment.lock.unlock();
  return result;
}

int env_set(const char *name, const char *value, int overwrite) {
  if (!env_name_valid(name)) {
    libc_errno = EINVAL;
    return -1;
  }

  cpp::string_view name_sv(name);
  cpp::string_view value_sv(value ? value : "");

  g_pcb.environment.lock.lock();
  env_check_divergence();

  int idx = env_find(name_sv);

  if (idx >= 0 && !overwrite) {
    // Name exists and overwrite not requested — success, no change.
    g_pcb.environment.lock.unlock();
    return 0;
  }

  // Allocate the new "NAME=VALUE" string.
  char *entry = env_make_entry(name_sv, value_sv);
  if (!entry) {
    g_pcb.environment.lock.unlock();
    libc_errno = ENOMEM;
    return -1;
  }

  if (idx >= 0) {
    // Replace existing entry.
    if (env_is_owned(idx))
      LIBC_NAMESPACE::free(g_pcb.environment.ptrs[idx]);
    g_pcb.environment.ptrs[idx] = entry;
    env_set_owned(idx, true);
  } else {
    // Append new entry — may need to grow.
    // We need count+1 for the entry plus one for the null terminator.
    if (g_pcb.environment.count + 1 >= g_pcb.environment.capacity) {
      int new_cap = g_pcb.environment.capacity < 16
                        ? 16
                        : g_pcb.environment.capacity * 2;
      if (!env_grow(new_cap)) {
        LIBC_NAMESPACE::free(entry);
        g_pcb.environment.lock.unlock();
        libc_errno = ENOMEM;
        return -1;
      }
    }
    g_pcb.environment.ptrs[g_pcb.environment.count] = entry;
    env_set_owned(g_pcb.environment.count, true);
    ++g_pcb.environment.count;
    g_pcb.environment.ptrs[g_pcb.environment.count] = nullptr;
  }

  env_sync();
  g_pcb.environment.lock.unlock();
  return 0;
}

int env_unset(const char *name) {
  if (!env_name_valid(name)) {
    libc_errno = EINVAL;
    return -1;
  }

  cpp::string_view name_sv(name);

  g_pcb.environment.lock.lock();
  env_check_divergence();

  // Remove ALL matching entries. Scan forward, compact in place.
  int write = 0;
  for (int read = 0; read < g_pcb.environment.count; ++read) {
    cpp::string_view entry(g_pcb.environment.ptrs[read]);
    bool match = entry.size() > name_sv.size() &&
                 entry[name_sv.size()] == '=' &&
                 env_name_equal_ignore_case(
                     entry.substr(0, name_sv.size()), name_sv);
    if (match) {
      if (env_is_owned(read))
        LIBC_NAMESPACE::free(g_pcb.environment.ptrs[read]);
      // Skip — don't copy to write position.
    } else {
      if (write != read) {
        g_pcb.environment.ptrs[write] = g_pcb.environment.ptrs[read];
        env_set_owned(write, env_is_owned(read));
      }
      ++write;
    }
  }
  g_pcb.environment.count = write;
  if (g_pcb.environment.ptrs)
    g_pcb.environment.ptrs[write] = nullptr;

  env_sync();
  g_pcb.environment.lock.unlock();
  return 0;
}

int env_put(char *string) {
  if (!string) {
    libc_errno = EINVAL;
    return -1;
  }

  // Find the '=' separator.
  cpp::string_view sv(string);
  size_t eq_pos = sv.find_first_of('=');
  if (eq_pos == cpp::string_view::npos) {
    libc_errno = EINVAL;
    return -1;
  }
  cpp::string_view name_sv = sv.substr(0, eq_pos);

  g_pcb.environment.lock.lock();
  env_check_divergence();

  int idx = env_find(name_sv);

  if (idx >= 0) {
    // Replace existing entry.
    if (env_is_owned(idx))
      LIBC_NAMESPACE::free(g_pcb.environment.ptrs[idx]);
    g_pcb.environment.ptrs[idx] = string;
    env_set_owned(idx, false); // Caller owns the string.
  } else {
    // Append new entry.
    if (g_pcb.environment.count + 1 >= g_pcb.environment.capacity) {
      int new_cap = g_pcb.environment.capacity < 16
                        ? 16
                        : g_pcb.environment.capacity * 2;
      if (!env_grow(new_cap)) {
        g_pcb.environment.lock.unlock();
        libc_errno = ENOMEM;
        return -1;
      }
    }
    g_pcb.environment.ptrs[g_pcb.environment.count] = string;
    env_set_owned(g_pcb.environment.count, false);
    ++g_pcb.environment.count;
    g_pcb.environment.ptrs[g_pcb.environment.count] = nullptr;
  }

  env_sync();
  g_pcb.environment.lock.unlock();
  return 0;
}

int env_clear() {
  g_pcb.environment.lock.lock();
  env_check_divergence();

  // Free all owned strings.
  for (int i = 0; i < g_pcb.environment.count; ++i) {
    if (env_is_owned(i))
      LIBC_NAMESPACE::free(g_pcb.environment.ptrs[i]);
  }

  // Reset to empty. Keep the allocation for reuse.
  g_pcb.environment.count = 0;
  if (g_pcb.environment.ptrs)
    g_pcb.environment.ptrs[0] = nullptr;

  // Zero the ownership bitmap.
  if (g_pcb.environment.capacity > 0) {
    size_t bitmap_bytes =
        (static_cast<size_t>(g_pcb.environment.capacity) + 7) / 8;
    __builtin_memset(env_bitmap(), 0, bitmap_bytes);
  }

  env_sync();
  g_pcb.environment.lock.unlock();
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// =========================================================================
// Fork reinit
// =========================================================================

void LIBC_NAMESPACE::internal::env_fork_reinit() {
  LIBC_NAMESPACE::g_pcb.environment.lock.reset_for_fork();
  // env_ptrs/env_count/env_capacity survive fork unchanged.
  // The child inherits the parent's environment snapshot.
  // Re-sync the environ export (child has its own copy of the BSS variable).
  LIBC_NAMESPACE::environ = LIBC_NAMESPACE::g_pcb.environment.ptrs;
}

LIBC_REGISTER_FORK_REINIT(env,
                          ::LIBC_NAMESPACE::internal::kForkPrioEnv,
                          &::LIBC_NAMESPACE::internal::env_fork_reinit)
