//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___FUNCTIONAL_MOVE_ONLY_FUNCTION_H
#define _LIBCPP___FUNCTIONAL_MOVE_ONLY_FUNCTION_H

#include <__assert>
#include <__config>
#include <__cstddef/nullptr_t.h>
#include <__functional/fast_forward.h>
#include <__functional/invoke.h>
#include <__functional/move_only_function_dispatch.h>
#include <__fwd/functional.h>
#include <__memory/addressof.h>
#include <__memory/construct_at.h>
#include <__new/allocate.h>
#include <__new/launder.h>
#include <__type_traits/conditional.h>
#include <__type_traits/decay.h>
#include <__type_traits/invoke.h>
#include <__type_traits/is_constructible.h>
#include <__type_traits/is_member_pointer.h>
#include <__type_traits/is_nothrow_constructible.h>
#include <__type_traits/is_pointer.h>
#include <__type_traits/is_reference.h>
#include <__type_traits/is_same.h>
#include <__type_traits/remove_cvref.h>
#include <__utility/element_count.h>
#include <__utility/exception_guard.h>
#include <__utility/exchange.h>
#include <__utility/forward.h>
#include <__utility/in_place.h>
#include <__utility/move.h>
#include <initializer_list>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

#if _LIBCPP_STD_VER >= 23

_LIBCPP_BEGIN_NAMESPACE_STD

namespace __move_only_function {

template <class>
inline constexpr bool __is_move_only_function = false;
template <class... _Signatures>
inline constexpr bool __is_move_only_function<move_only_function<_Signatures...>> = true;

template <class>
struct __signature {
  using type = void;
};

// The manager owns the target's lifetime. Unlike __small_buffer, this storage
// can contain objects whose moves and destructors have observable effects.
class __storage {
protected:
  alignas(void*) mutable unsigned char __buffer_[3 * sizeof(void*)];
  const __dispatch_base* __table_ = nullptr;

  template <class _Tp>
  static constexpr bool __small =
      sizeof(_Tp) <= sizeof(__buffer_) && alignof(_Tp) <= alignof(void*) && is_nothrow_move_constructible_v<_Tp>;

  template <class _Tp>
  _LIBCPP_HIDE_FROM_ABI static _Tp* __get(void* __buffer) noexcept {
    if constexpr (__small<_Tp>)
      return std::launder(static_cast<_Tp*>(__buffer));
    else
      return *std::launder(static_cast<_Tp**>(__buffer));
  }

  template <class _Tp>
  _LIBCPP_HIDE_FROM_ABI static void __manage(void* __dest, void* __source) noexcept {
    _Tp* __target = __get<_Tp>(__source);
    if (__dest) {
      if constexpr (!__small<_Tp>) {
        std::construct_at(static_cast<_Tp**>(__dest), __target);
        return;
      } else {
#  if __has_builtin(__builtin_is_bitwise_relocatable)
        if constexpr (__builtin_is_bitwise_relocatable(_Tp)) {
          // Transfers always use distinct buffers and an unoccupied destination.
          __builtin_trivially_relocate(static_cast<_Tp*>(__dest), __target, 1, true);
          return;
        }
#    if __has_feature(ptrauth_intrinsics)
        else if constexpr (!__builtin_is_cpp_trivially_relocatable(_Tp) &&
                           requires(_Tp* __p) { __builtin_trivially_relocate(__p, __p, 0); }) {
          // Exclude the legacy relocation domain. Acceptance then establishes
          // the extension's eligibility, including authenticated pointer fields.
          __builtin_trivially_relocate(static_cast<_Tp*>(__dest), __target, 1);
          return;
        }
#    endif
#  endif
        std::construct_at(static_cast<_Tp*>(__dest), std::move(*__target));
      }
    }
    std::destroy_at(__target);
    if constexpr (!__small<_Tp>)
      std::__libcpp_deallocate<_Tp>(__target, __element_count(1));
  }

  template <class _Tp, class... _Args>
  _LIBCPP_HIDE_FROM_ABI void __construct(_Args&&... __args) {
    if constexpr (__small<_Tp>) {
      std::construct_at(reinterpret_cast<_Tp*>(__buffer_), std::forward<_Args>(__args)...);
    } else {
      _Tp* __target = std::__libcpp_allocate<_Tp>(__element_count(1));
      auto __guard  = std::__make_exception_guard([&] { std::__libcpp_deallocate<_Tp>(__target, __element_count(1)); });
      std::construct_at(__target, std::forward<_Args>(__args)...);
      std::construct_at(reinterpret_cast<_Tp**>(__buffer_), __target);
      __guard.__complete();
    }
  }

  // The destination has no target; moving also disarms the source's destructor.
  _LIBCPP_HIDE_FROM_ABI void __take(__storage& __other) noexcept {
    __table_ = std::exchange(__other.__table_, nullptr);
    if (__table_)
      __table_->__manage_(__buffer_, __other.__buffer_);
  }

  _LIBCPP_HIDE_FROM_ABI void __reset() noexcept {
    if (auto* __table = std::exchange(__table_, nullptr))
      __table->__manage_(nullptr, __buffer_);
  }

  _LIBCPP_HIDE_FROM_ABI __storage() noexcept = default;
  _LIBCPP_HIDE_FROM_ABI __storage(__storage&& __other) noexcept : __table_(std::exchange(__other.__table_, nullptr)) {
    if (__table_)
      __table_->__manage_(__buffer_, __other.__buffer_);
  }
  _LIBCPP_HIDE_FROM_ABI ~__storage() { __reset(); }

  _LIBCPP_HIDE_FROM_ABI void __swap(__storage& __other) noexcept {
    if (this == std::addressof(__other))
      return;
    if (!__table_) {
      __take(__other);
    } else if (!__other.__table_) {
      __other.__take(*this);
    } else {
      __storage __temp(std::move(__other));
      __other.__take(*this);
      __take(__temp);
    }
  }
};

enum class __ref_qualifier { __none, __lvalue, __rvalue };

template <class _Self, class _Signature, bool _Const, __ref_qualifier _Ref>
class __function;

template <class _Self, class _Rp, class... _Args, bool _Noexcept, bool _Const, __ref_qualifier _Ref>
class __function<_Self, _Rp(_Args...) noexcept(_Noexcept), _Const, _Ref> : public __storage {
  template <class _Tp>
  using __cv_target = __conditional_t<_Const, const _Tp, _Tp>;
  template <class _Tp>
  using __inv_target = __conditional_t<_Ref == __ref_qualifier::__rvalue, __cv_target<_Tp>&&, __cv_target<_Tp>&>;
  template <class _Tp>
  using __qualified_target = __conditional_t<_Ref == __ref_qualifier::__none, __cv_target<_Tp>, __inv_target<_Tp>>;

  template <class _Tp>
  static constexpr bool __callable =
      _Noexcept ? is_nothrow_invocable_r_v<_Rp, __qualified_target<_Tp>, _Args...> &&
                      is_nothrow_invocable_r_v<_Rp, __inv_target<_Tp>, _Args...>
                : is_invocable_r_v<_Rp, __qualified_target<_Tp>, _Args...> &&
                      is_invocable_r_v<_Rp, __inv_target<_Tp>, _Args...>;

  template <bool _UseValue>
  using __invoker = _Rp(void*,
                        typename std::__function::__small_forward<_Args, _UseValue>::type...) noexcept(_Noexcept);

  template <class _Tp, bool _UseValue>
  _LIBCPP_HIDE_FROM_ABI static _Rp
  __invoke(void* __buffer,
           typename std::__function::__small_forward<_Args, _UseValue>::type... __args) noexcept(_Noexcept) {
    return std::invoke_r<_Rp>(static_cast<__inv_target<_Tp>>(*__get<_Tp>(__buffer)), std::forward<_Args>(__args)...);
  }

  template <class _Tp, bool _UseValue, class... _Init>
  _LIBCPP_HIDE_FROM_ABI void __init(_Init&&... __args) {
    constexpr auto __mode = _UseValue ? __argument_passing::__value : __argument_passing::__reference;
    static constexpr __dispatch_table<__invoker<_UseValue>, __mode> __table{__manage<_Tp>, __invoke<_Tp, _UseValue>};
    __construct<_Tp>(std::forward<_Init>(__args)...);
    __table_ = &__table;
  }

public:
  _LIBCPP_HIDE_FROM_ABI __function() noexcept = default;
  _LIBCPP_HIDE_FROM_ABI __function(nullptr_t) noexcept {}
  _LIBCPP_HIDE_FROM_ABI __function(__function&&) noexcept = default;

  // Include the argument mode in constructor specializations as well as tables:
  // the same target can be constructed with incomplete arguments in another TU.
  template <
      class _Fp,
      class _VT = decay_t<_Fp>,
      bool _UseValue =
          (std::__function::__can_use_small_value<_Args, is_reference_v<_Args> || requires { sizeof(_Args); }>() &&
           ...)>
    requires(!is_same_v<__remove_cvref_t<_Fp>, _Self> && !__is_inplace_type<_Fp>::value && __callable<_VT>)
  _LIBCPP_HIDE_FROM_ABI __function(_Fp&& __f) {
    static_assert(is_constructible_v<_VT, _Fp>, "move_only_function target must be constructible from the argument");
    if constexpr (is_constructible_v<_VT, _Fp>) {
      if constexpr (is_pointer_v<_VT> || is_member_pointer_v<_VT> || __is_move_only_function<_VT>) {
        if (!__f)
          return;
      }
      // Retain the original thunk's qualifications, including after noexcept weakening.
      if constexpr (is_same_v<typename __signature<_VT>::type, _Rp(_Args...)>)
        __take(__f);
      else
        __init<_VT, _UseValue>(std::forward<_Fp>(__f));
    }
  }

  template <
      class _Tp,
      class... _Init,
      class _VT = decay_t<_Tp>,
      bool _UseValue =
          (std::__function::__can_use_small_value<_Args, is_reference_v<_Args> || requires { sizeof(_Args); }>() &&
           ...)>
    requires(is_constructible_v<_VT, _Init...> && __callable<_VT>)
  _LIBCPP_HIDE_FROM_ABI explicit __function(in_place_type_t<_Tp>, _Init&&... __args) {
    static_assert(is_same_v<_Tp, _VT>, "move_only_function in-place target must be a decayed type");
    __init<_VT, _UseValue>(std::forward<_Init>(__args)...);
  }

  template <
      class _Tp,
      class _Up,
      class... _Init,
      class _VT = decay_t<_Tp>,
      bool _UseValue =
          (std::__function::__can_use_small_value<_Args, is_reference_v<_Args> || requires { sizeof(_Args); }>() &&
           ...)>
    requires(is_constructible_v<_VT, initializer_list<_Up>&, _Init...> && __callable<_VT>)
  _LIBCPP_HIDE_FROM_ABI explicit __function(in_place_type_t<_Tp>, initializer_list<_Up> __il, _Init&&... __args) {
    static_assert(is_same_v<_Tp, _VT>, "move_only_function in-place target must be a decayed type");
    __init<_VT, _UseValue>(__il, std::forward<_Init>(__args)...);
  }

protected:
  _LIBCPP_HIDE_FROM_ABI _Rp __call(std::__function::__fast_forward<_Args>... __args) const noexcept(_Noexcept) {
    _LIBCPP_ASSERT_NON_NULL(__table_ != nullptr, "move_only_function has no target");
    constexpr bool __complete = ((is_reference_v<_Args> || requires { sizeof(_Args); }) && ...);
    using _Dispatch           = __dispatch_call<__invoker<false>, __invoker<__complete>>;
    return _Dispatch::template __call<(std::__function::__can_use_small_value<_Args, __complete>() && ...)>(
        __table_, __buffer_, std::forward<_Args>(__args)...);
  }
};

} // namespace __move_only_function

// Keep the public call operator's exact signature while sharing storage and construction.
#  define _LIBCPP_MOVE_ONLY_FUNCTION(_CV, _REF, _CONST, _REF_KIND)                                                     \
    template <class _Rp, class... _Args, bool _Noexcept>                                                               \
    class move_only_function<_Rp(_Args...) _CV _REF noexcept(_Noexcept)>                                               \
        : private __move_only_function::__function< move_only_function<_Rp(_Args...) _CV _REF noexcept(_Noexcept)>,    \
                                                    _Rp(_Args...) noexcept(_Noexcept),                                 \
                                                    _CONST,                                                            \
                                                    __move_only_function::__ref_qualifier::_REF_KIND> {                \
      using _Base =                                                                                                    \
          __move_only_function::__function< move_only_function,                                                        \
                                            _Rp(_Args...) noexcept(_Noexcept),                                         \
                                            _CONST,                                                                    \
                                            __move_only_function::__ref_qualifier::_REF_KIND>;                         \
      template <class, class, bool, __move_only_function::__ref_qualifier>                                             \
      friend class __move_only_function::__function;                                                                   \
                                                                                                                       \
    public:                                                                                                            \
      using result_type = _Rp;                                                                                         \
      using _Base::_Base;                                                                                              \
      _LIBCPP_HIDE_FROM_ABI move_only_function() noexcept {}                                                           \
      _LIBCPP_HIDE_FROM_ABI move_only_function(move_only_function&&) noexcept = default;                               \
      _LIBCPP_HIDE_FROM_ABI move_only_function& operator=(move_only_function&& __f) {                                  \
        move_only_function(std::move(__f)).swap(*this);                                                                \
        return *this;                                                                                                  \
      }                                                                                                                \
      _LIBCPP_HIDE_FROM_ABI move_only_function& operator=(nullptr_t) noexcept {                                        \
        this->__reset();                                                                                               \
        return *this;                                                                                                  \
      }                                                                                                                \
      template <class _Fp>                                                                                             \
      _LIBCPP_HIDE_FROM_ABI move_only_function& operator=(_Fp&& __f) {                                                 \
        move_only_function(std::forward<_Fp>(__f)).swap(*this);                                                        \
        return *this;                                                                                                  \
      }                                                                                                                \
      _LIBCPP_HIDE_FROM_ABI explicit operator bool() const noexcept { return this->__table_ != nullptr; }              \
      _LIBCPP_HIDE_FROM_ABI _Rp operator()(_Args... __args) _CV _REF noexcept(_Noexcept) {                             \
        return this->__call(std::forward<_Args>(__args)...);                                                           \
      }                                                                                                                \
      _LIBCPP_HIDE_FROM_ABI void swap(move_only_function& __other) noexcept { this->__swap(__other); }                 \
      _LIBCPP_HIDE_FROM_ABI friend void swap(move_only_function& __x, move_only_function& __y) noexcept {              \
        __x.swap(__y);                                                                                                 \
      }                                                                                                                \
      _LIBCPP_HIDE_FROM_ABI friend bool operator==(const move_only_function& __f, nullptr_t) noexcept { return !__f; } \
    };                                                                                                                 \
    template <class _Rp, class... _Args, bool _Noexcept>                                                               \
    struct __move_only_function::__signature<move_only_function<_Rp(_Args...) _CV _REF noexcept(_Noexcept)>> {         \
      using type = _Rp(_Args...);                                                                                      \
    }

_LIBCPP_MOVE_ONLY_FUNCTION(, , false, __none);
_LIBCPP_MOVE_ONLY_FUNCTION(, &, false, __lvalue);
_LIBCPP_MOVE_ONLY_FUNCTION(, &&, false, __rvalue);
_LIBCPP_MOVE_ONLY_FUNCTION(const, , true, __none);
_LIBCPP_MOVE_ONLY_FUNCTION(const, &, true, __lvalue);
_LIBCPP_MOVE_ONLY_FUNCTION(const, &&, true, __rvalue);

#  undef _LIBCPP_MOVE_ONLY_FUNCTION

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP_STD_VER >= 23

#endif // _LIBCPP___FUNCTIONAL_MOVE_ONLY_FUNCTION_H
