//===-- Reserve-and-commit atexit callback list for Windows --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Growable array backed by CommitRegion. Used as the ExitCallbackList on
// Windows, replacing BlockStore to avoid the malloc → atexit circular
// dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_EXIT_CALLBACKS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_EXIT_CALLBACKS_H

#include "src/__support/OSUtil/windows/memory/commit_region.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {

// Growable array backed by CommitRegion (reserve-then-commit VA).
// Trivially constructible (constinit-safe) with lazy init on first push.
template <typename T> class CommitVector {
  internal::CommitRegion region_;
  T *data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0; // entries backed by committed pages

  // Reserve 256KB — room for ~10,000 AtExitUnit entries.
  static constexpr size_t RESERVE_BYTES = 256 * 1024;

  LIBC_INLINE bool ensure_init() {
    if (data_)
      return true;
    const size_t ps = windows::get_cached_page_size();
    if (!region_.init(RESERVE_BYTES, ps))
      return false;
    data_ = region_.as<T>();
    capacity_ = ps / sizeof(T);
    return true;
  }

  // Commit one more page of entries.
  LIBC_INLINE bool grow() {
    const size_t ps = windows::get_cached_page_size();
    size_t new_end = (capacity_ + ps / sizeof(T)) * sizeof(T);
    if (!region_.ensure_committed(new_end))
      return false;
    capacity_ = new_end / sizeof(T);
    return true;
  }

public:
  LIBC_INLINE constexpr CommitVector() = default;
  LIBC_INLINE ~CommitVector() = default;

  class Iterator {
    T *ptr_;

  public:
    LIBC_INLINE constexpr Iterator(T *p) : ptr_(p) {}
    LIBC_INLINE T &operator*() { return *ptr_; }
    LIBC_INLINE Iterator &operator++() { ++ptr_; return *this; }
    LIBC_INLINE bool operator==(const Iterator &rhs) const {
      return ptr_ == rhs.ptr_;
    }
    LIBC_INLINE bool operator!=(const Iterator &rhs) const {
      return ptr_ != rhs.ptr_;
    }
  };

  [[nodiscard]] LIBC_INLINE bool push_back(const T &value) {
    if (!ensure_init())
      return false;
    if (size_ >= capacity_ && !grow())
      return false;
    data_[size_++] = value;
    return true;
  }

  LIBC_INLINE T &back() { return data_[size_ - 1]; }

  LIBC_INLINE void pop_back() {
    if (size_ > 0)
      --size_;
  }

  LIBC_INLINE bool empty() const { return size_ == 0; }

  LIBC_INLINE Iterator begin() { return Iterator(data_); }
  LIBC_INLINE Iterator end() { return Iterator(data_ + size_); }

  LIBC_INLINE static void destroy(CommitVector *v) {
    v->region_.destroy();
    v->data_ = nullptr;
    v->size_ = 0;
    v->capacity_ = 0;
  }
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_EXIT_CALLBACKS_H
