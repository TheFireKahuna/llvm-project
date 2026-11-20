//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On Windows Itanium and NT-POSIX, the vtables of the classes in
// private_typeinfo.h that type_info objects have, for an image that links
// libc++abi's DLL: a member of the DLL's import library, from which an image
// takes the ones its type_info objects name. Each is the vtable that
// private_typeinfo.cpp emits, word for word, with the DLL's type_info and
// functions as its words, in a COMDAT that libc++abi's own vtable shares and
// that the image does not export.
//
//===----------------------------------------------------------------------===//

#if defined(_WIN32_ITANIUM) || defined(__NTPOSIX__)

// clang-format off
#  define NAME(Prefix, Class, Suffix) Prefix "10__cxxabiv1" Class Suffix
#  define CAN_CATCH(Class) ", " NAME("_ZNK", Class, "9can_catchEPKNS_16__shim_type_infoERPv")
#  define SEARCHES(Class)                                                                                              \
    ", " NAME("_ZNK", Class, "16search_above_dstEPNS_19__dynamic_cast_infoEPKvS4_ib")                                  \
    ", " NAME("_ZNK", Class, "16search_below_dstEPNS_19__dynamic_cast_infoEPKvib")                                     \
    ", " NAME("_ZNK", Class, "27has_unambiguous_public_baseEPNS_10catch_infoEPvi")

// The offset to top, the type_info, the complete destructor, which is the
// base's, the deleting destructor, the two unused entries, and Entries.
#  define VTABLE(Class, Entries)                                                                                       \
    __asm__(".section .rdata,\"dr\",discard," NAME("_ZTVN", Class, "E") "\n"                                           \
            ".globl " NAME("_ZTVN", Class, "E") "\n"                                                                   \
            ".p2align 6\n"                                                                                             \
            NAME("_ZTVN", Class, "E") ":\n"                                                                            \
            ".quad 0, " NAME("_ZTIN", Class, "E")                                                                      \
            ", _ZN10__cxxabiv116__shim_type_infoD2Ev, " NAME("_ZN", Class, "D0Ev")                                     \
            ", _ZNK10__cxxabiv116__shim_type_info5noop1Ev, _ZNK10__cxxabiv116__shim_type_info5noop2Ev" Entries "\n");
// clang-format on
#  define LEAF(Class) VTABLE(Class, CAN_CATCH(Class))
#  define DERIVED(Class) VTABLE(Class, CAN_CATCH("17__class_type_info") SEARCHES(Class))

LEAF("23__fundamental_type_info")
LEAF("17__array_type_info")
LEAF("20__function_type_info")
LEAF("16__enum_type_info")
LEAF("19__pointer_type_info")
LEAF("29__pointer_to_member_type_info")
DERIVED("17__class_type_info")
DERIVED("20__si_class_type_info")
DERIVED("21__vmi_class_type_info")

#endif
