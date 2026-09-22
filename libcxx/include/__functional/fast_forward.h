//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___FUNCTIONAL_FAST_FORWARD_H
#define _LIBCPP___FUNCTIONAL_FAST_FORWARD_H

#include <__config>
#include <__type_traits/conditional.h>
#include <__type_traits/is_reference.h>
#include <__type_traits/is_scalar.h>
#include <__type_traits/is_trivially_constructible.h>
#include <__type_traits/is_trivially_destructible.h>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD
namespace __function {

#ifndef _LIBCPP_CXX03_LANG
// Pass scalars in registers without introducing copies of class-type arguments.
template <class _Tp>
using __fast_forward _LIBCPP_NODEBUG = __conditional_t<is_scalar<_Tp>::value, _Tp, _Tp&&>;
#endif

#if _LIBCPP_STD_VER >= 23
template <class _Tp>
_LIBCPP_HIDE_FROM_ABI constexpr bool __use_small_value() {
  if constexpr (!is_reference_v<_Tp> && !is_scalar_v<_Tp>)
    // These sizes also avoid indirect aggregate arguments on the Windows x64 ABI.
    // Keep the candidate signature independent of compiler relocation support.
    return sizeof(_Tp) <= sizeof(void*) && (sizeof(_Tp) & (sizeof(_Tp) - 1)) == 0 &&
           is_trivially_copy_constructible_v<_Tp> && is_trivially_move_constructible_v<_Tp> &&
           is_trivially_destructible_v<_Tp>;
  return false;
}

// Permission to copy arguments is separate from their candidate signature.
// In particular, trivial construction alone does not exclude volatile fields.
template <class _Tp, bool _Complete>
_LIBCPP_HIDE_FROM_ABI constexpr bool __can_use_small_value() {
  if constexpr (!_Complete)
    return false;
  else if constexpr (!__use_small_value<_Tp>())
    return true;
#  if __has_builtin(__builtin_is_bitwise_relocatable)
  else
    return __builtin_is_bitwise_relocatable(_Tp);
#  else
  else
    return false;
#  endif
}

template <class _Tp, bool _UseValue>
struct __small_forward {
  using type = __fast_forward<_Tp>;
};

template <class _Tp>
struct __small_forward<_Tp, true> {
  using type = __conditional_t<__use_small_value<_Tp>(), _Tp, __fast_forward<_Tp>>;
};
#endif

} // namespace __function
_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___FUNCTIONAL_FAST_FORWARD_H
