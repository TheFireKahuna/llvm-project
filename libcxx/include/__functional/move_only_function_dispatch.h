//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___FUNCTIONAL_MOVE_ONLY_FUNCTION_DISPATCH_H
#define _LIBCPP___FUNCTIONAL_MOVE_ONLY_FUNCTION_DISPATCH_H

#include <__config>
#include <__type_traits/is_same.h>
#include <__utility/forward.h>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

#if _LIBCPP_STD_VER >= 23

_LIBCPP_BEGIN_NAMESPACE_STD

namespace __move_only_function {

enum class __argument_passing { __reference, __value };

// Keeping management independent of the call signature allows the wrapper to
// hold incomplete argument types. Instantiate the typed table only when forming
// or invoking a target, not when determining the wrapper's layout.
struct __dispatch_base {
  using __manager = void (*)(void*, void*) noexcept;

  __manager __manage_;
  __argument_passing __arguments_;
};

// _Invoker includes the storage parameter and any adjusted argument types.
// Recover the matching typed view before calling; do not cast function pointers.
template <class _Invoker>
struct __dispatch_view;

template <class _Rp, class... _Args>
struct __dispatch_view<_Rp(_Args...)> : __dispatch_base {
  _Rp (*__invoke_)(_Args...);

  _LIBCPP_HIDE_FROM_ABI constexpr __dispatch_view(
      __manager __manage, __argument_passing __arguments, _Rp (*__invoke)(_Args...))
      : __dispatch_base{__manage, __arguments}, __invoke_(__invoke) {}
};

// The base entry permits noexcept weakening without an adapter or another
// per-wrapper pointer. Both entries are initialized from the nonthrowing thunk.
template <class _Rp, class... _Args>
struct __dispatch_view<_Rp(_Args...) noexcept> : __dispatch_view<_Rp(_Args...)> {
  _Rp (*__invoke_)(_Args...) noexcept;

  _LIBCPP_HIDE_FROM_ABI constexpr __dispatch_view(
      __dispatch_base::__manager __manage, __argument_passing __arguments, _Rp (*__invoke)(_Args...) noexcept)
      : __dispatch_view<_Rp(_Args...)>(__manage, __arguments, __invoke), __invoke_(__invoke) {}
};

// Stamp the argument mode into the table's type and metadata. Factories must
// likewise distinguish modes in their template arguments, not cache a query
// whose result can change when a type becomes complete.
template <class _Invoker, __argument_passing _Arguments>
struct __dispatch_table : __dispatch_view<_Invoker> {
  _LIBCPP_HIDE_FROM_ABI constexpr __dispatch_table(__dispatch_base::__manager __manage, _Invoker* __invoke)
      : __dispatch_view<_Invoker>(__manage, _Arguments, __invoke) {}
};

template <class _ReferenceInvoker, class _ValueInvoker>
struct __dispatch_call;

// The invokers have the same return type and exception specification. Value
// adjustment must preserve the public call's argument and lifetime semantics.
template <class _ReferenceInvoker, class _Rp, class... _Args, bool _Noexcept>
struct __dispatch_call<_ReferenceInvoker, _Rp(_Args...) noexcept(_Noexcept)> {
private:
  // Keep reference materialization out of the value path's stack frame. Inlining
  // this fallback can otherwise impose spills and stack protection on both paths.
  _LIBCPP_HIDE_FROM_ABI _LIBCPP_NOINLINE static _Rp
  __call_reference(_Args... __args, const __dispatch_base* __table) noexcept(_Noexcept) {
    return static_cast<const __dispatch_view<_ReferenceInvoker>*>(__table)->__invoke_(std::forward<_Args>(__args)...);
  }

public:
  _LIBCPP_HIDE_FROM_ABI static _Rp __call(const __dispatch_base* __table, _Args... __args) noexcept(_Noexcept) {
    using _ValueInvoker = _Rp(_Args...) noexcept(_Noexcept);
    if constexpr (!is_same_v<_ReferenceInvoker, _ValueInvoker>) {
      if (__table->__arguments_ == __argument_passing::__reference)
        return __call_reference(std::forward<_Args>(__args)..., __table);
    }
    return static_cast<const __dispatch_view<_ValueInvoker>*>(__table)->__invoke_(std::forward<_Args>(__args)...);
  }
};

} // namespace __move_only_function

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP_STD_VER >= 23

#endif // _LIBCPP___FUNCTIONAL_MOVE_ONLY_FUNCTION_DISPATCH_H
