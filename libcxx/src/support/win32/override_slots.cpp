//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// What a program's link needs to write the override table, as absolute
// symbols: the format of the table that the library reads, and for each
// overridable default of the library its slot and, for one that the standard
// specifies as calling another, that other's slot. This object is a regular
// member of the import library and of the static library, which a link loads
// only when it replaces a default, so the slots it writes are those of the
// library that checks them.

#include <__config>
#include <cstddef>

#include "include/overridable_function.h"

#if _LIBCPP_HAS_OVERRIDE_TABLE

// The ABI names below are those of these LLP64 targets and of the default ABI
// namespace.
static_assert(sizeof(std::size_t) == sizeof(unsigned long long) && sizeof(long) == 4);
static_assert(__builtin_strcmp(_LIBCPP_TOSTRING(_LIBCPP_ABI_NAMESPACE), "__1") == 0);

// A forwarding target with this bit takes the forwarding default's third
// argument as its second: a sized aligned default drops its size.
#  define _LIBCPP_OVERRIDE_DROPS_SECOND 0x10000

#  define _LIBCPP_OVERRIDE_SYMBOL(__name, __value)                                                                     \
    ".globl \"" __name "\"\n.set \"" __name "\", " _LIBCPP_TOSTRING(__value) "\n"
#  define _LIBCPP_OVERRIDE_SLOT_OF(__abi_name, __slot) _LIBCPP_OVERRIDE_SYMBOL("__ovr_slot$" __abi_name, __slot)
#  define _LIBCPP_OVERRIDE_FORWARD_OF(__abi_name, __target) _LIBCPP_OVERRIDE_SYMBOL("__ovr_fwd$" __abi_name, __target)

__asm__(
    // clang-format off
    _LIBCPP_OVERRIDE_SYMBOL("__ovr_version", _LIBCPP_OVERRIDE_TABLE_VERSION)

    _LIBCPP_OVERRIDE_SLOT_OF("_Znwy", _LIBCPP_OVERRIDE_SLOT_NEW)
    _LIBCPP_OVERRIDE_SLOT_OF("_Znay", _LIBCPP_OVERRIDE_SLOT_NEW_ARRAY)
    _LIBCPP_OVERRIDE_FORWARD_OF("_Znay", _LIBCPP_OVERRIDE_SLOT_NEW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdlPv", _LIBCPP_OVERRIDE_SLOT_DELETE)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdlPvy", _LIBCPP_OVERRIDE_SLOT_DELETE_SIZED)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZdlPvy", _LIBCPP_OVERRIDE_SLOT_DELETE)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdaPv", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZdaPv", _LIBCPP_OVERRIDE_SLOT_DELETE)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdaPvy", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY_SIZED)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZdaPvy", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZnwySt11align_val_t", _LIBCPP_OVERRIDE_SLOT_NEW_ALIGNED)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdlPvSt11align_val_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ALIGNED)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZnaySt11align_val_t", _LIBCPP_OVERRIDE_SLOT_NEW_ARRAY_ALIGNED)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZnaySt11align_val_t", _LIBCPP_OVERRIDE_SLOT_NEW_ALIGNED)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdlPvySt11align_val_t", _LIBCPP_OVERRIDE_SLOT_DELETE_SIZED_ALIGNED)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZdlPvySt11align_val_t",
                                _LIBCPP_OVERRIDE_SLOT_DELETE_ALIGNED | _LIBCPP_OVERRIDE_DROPS_SECOND)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdaPvSt11align_val_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY_ALIGNED)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZdaPvSt11align_val_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ALIGNED)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdaPvySt11align_val_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY_SIZED_ALIGNED)
    _LIBCPP_OVERRIDE_FORWARD_OF("_ZdaPvySt11align_val_t",
                                _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY_ALIGNED | _LIBCPP_OVERRIDE_DROPS_SECOND)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZnwyRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_NEW_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZnayRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_NEW_ARRAY_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZnwySt11align_val_tRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_NEW_ALIGNED_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZnaySt11align_val_tRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_NEW_ARRAY_ALIGNED_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdlPvRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_DELETE_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdaPvRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdlPvSt11align_val_tRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ALIGNED_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZdaPvSt11align_val_tRKSt9nothrow_t", _LIBCPP_OVERRIDE_SLOT_DELETE_ARRAY_ALIGNED_NOTHROW)
    _LIBCPP_OVERRIDE_SLOT_OF("_ZNSt3__122__libcpp_verbose_abortEPKcz", _LIBCPP_OVERRIDE_SLOT_VERBOSE_ABORT)
    // clang-format on
);

#endif // _LIBCPP_HAS_OVERRIDE_TABLE
