//===-- Wile E. Coyote header <custom.h> --===//
//
// Caveat emptor.
// I never studied law.
//
//===---------------------------------------------------------------------===//

#ifndef _LLVM_LIBC_CUSTOM_H
#define _LLVM_LIBC_CUSTOM_H

#if !defined(_WIN32) || defined(__NTPOSIX__)

#include "__llvm-libc-common.h"
#include "llvm-libc-types/meep.h"
#include "llvm-libc-types/road.h"

__BEGIN_C_DECLS

__LIBC_FUNC_IMPORT road runner(meep, meep) __NOEXCEPT;

__END_C_DECLS

#endif // !defined(_WIN32) || defined(__NTPOSIX__)
#endif // _LLVM_LIBC_CUSTOM_H

#if defined(_WIN32) && !defined(__NTPOSIX__) && __has_include_next(<custom.h>)
#include_next <custom.h>
#endif
