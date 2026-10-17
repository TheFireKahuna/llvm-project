//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On Windows Itanium and NT-POSIX, the copies of libc++abi's type_info
// vtables that every image linked against its DLL holds, as a member of the
// DLL's import library. A type_info object points at its own image's copy,
// which costs the image a relocation, where importing the vtable would cost
// each type_info object an import slot of its own.
//
// Each copy is the vtable private_typeinfo.cpp emits, word for word: the
// address point 16 bytes into a 64-byte line, then the RTTI word, which the
// import library binds to the class's qualified type_info, so that a DLL with
// another layout of these classes fails to load, and the entries, which are
// the DLL's exported functions. Each copy is a COMDAT any, hidden to the
// image, which libc++abi's own vtable folds into where both are linked.
//
//===----------------------------------------------------------------------===//

#if defined(_WIN32_ITANIUM) || defined(__NTPOSIX__)

#  define VTABLE_HEAD(Name)                                                    \
    ".section .rdata,\"dr\",discard," Name "\n"                                \
    ".globl " Name "\n"                                                        \
    ".p2align 6\n"                                                             \
    ".linkpin " Name ", 6, 0\n" Name ":\n"                                     \
    ".quad 0\n"

// The entries that every kind shares: the destructors, whose complete form is
// the shim's base destructor, and the two padding entries.
#  define SHIM_ENTRIES(DeletingDestructor)                                     \
    ".quad _ZN10__cxxabiv116__shim_type_infoD2Ev\n"                            \
    ".quad " DeletingDestructor "\n"                                           \
    ".quad _ZNK10__cxxabiv116__shim_type_info5noop1Ev\n"                       \
    ".quad _ZNK10__cxxabiv116__shim_type_info5noop2Ev\n"

#  define CAN_CATCH(Class) ".quad _ZNK10__cxxabiv1" Class "9can_catchEPKNS_16__shim_type_infoERPv\n"

#  define LEAF_VTABLE(Class)                                                   \
    VTABLE_HEAD("_ZTVN10__cxxabiv1" Class "E")                                 \
    ".quad _ZTIN10__cxxabiv1" Class "E\n" SHIM_ENTRIES("_ZN10__cxxabiv1" Class "D0Ev") CAN_CATCH(Class)

#  define SEARCH_ENTRIES(Class)                                                \
    ".quad _ZNK10__cxxabiv1" Class "16search_above_dstEPNS_19__dynamic_cast_infoEPKvS4_ib\n" \
    ".quad _ZNK10__cxxabiv1" Class "16search_below_dstEPNS_19__dynamic_cast_infoEPKvib\n"    \
    ".quad _ZNK10__cxxabiv1" Class "27has_unambiguous_public_baseEPNS_19__dynamic_cast_infoEPvi\n"

#  define CLASS_VTABLE(Class)                                                  \
    VTABLE_HEAD("_ZTVN10__cxxabiv1" Class "E")                                 \
    ".quad _ZTIN10__cxxabiv1" Class "E\n" SHIM_ENTRIES("_ZN10__cxxabiv1" Class "D0Ev") CAN_CATCH("17__class_type_info") SEARCH_ENTRIES(Class)

// Every definition here is hidden: the copies are the image's own.
__asm__(".linkbindings\n"
        LEAF_VTABLE("23__fundamental_type_info")
        LEAF_VTABLE("17__array_type_info")
        LEAF_VTABLE("20__function_type_info")
        LEAF_VTABLE("16__enum_type_info")
        CLASS_VTABLE("17__class_type_info")
        CLASS_VTABLE("20__si_class_type_info")
        CLASS_VTABLE("21__vmi_class_type_info")
        LEAF_VTABLE("19__pointer_type_info")
        LEAF_VTABLE("29__pointer_to_member_type_info"));

#endif
