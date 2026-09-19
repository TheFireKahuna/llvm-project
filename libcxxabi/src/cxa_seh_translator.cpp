//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Structured exceptions as C++ exceptions. While the system looks for a
// handler, libunwind asks this runtime once per C++ frame for the object to
// offer that frame. A translator installed with _set_se_translator (MSVC's
// API) answers by throwing an object of its own type. Without one, or when
// the translator returns, the object is of a type no catch clause can name,
// so catch (...) alone takes it, as in MSVC; unlike a foreign exception it
// can be caught while another exception is being handled, sits in an
// exception_ptr, and is raised again as the structured exception by throw;.
//
//===----------------------------------------------------------------------===//

#include "cxxabi.h"

#if defined(__SEH__) && !defined(__USING_SJLJ_EXCEPTIONS__) && defined(_WIN32)

#include "cxa_exception.h"
#include <exception>
#include <typeinfo>
#include <unwind.h>

#if defined(__NTPOSIX__)
#include <sys/ntabi.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
typedef void(__cdecl *_se_translator_function)(unsigned int,
                                                struct _EXCEPTION_POINTERS *);
_LIBCXXABI_FUNC_VIS _se_translator_function __cdecl
_set_se_translator(_se_translator_function);
}

namespace __cxxabiv1 {
namespace {

thread_local _se_translator_function seh_translator = nullptr;

// The object for a structured exception no translator claimed.
struct seh_exception {
  EXCEPTION_RECORD record;
};

void destroy_seh_exception(void *) {}

__cxa_exception *header_of(_Unwind_Exception *exc) {
  return reinterpret_cast<__cxa_exception *>(exc + 1) - 1;
}

// A frame libunwind offered the object declined it. Handing it over counted
// it as in flight, as a throw does; undo that, then drop the reference.
void declined(_Unwind_Reason_Code reason, _Unwind_Exception *exc) {
  if (reason != _URC_FOREIGN_EXCEPTION_CAUGHT)
    std::terminate();
  __cxa_get_globals()->uncaughtExceptions -= 1;
  __cxa_decrement_exception_refcount(exc + 1);
}

// The object leaves with the one reference a thrown object carries and is
// in flight from here on: the handler that takes it ends it as a throw's
// handler would, and a frame that declines it calls declined().
_Unwind_Exception *hand_over(void *thrown) {
  __cxa_exception *header =
      static_cast<__cxa_exception *>(thrown) - 1;
  header->unwindHeader.exception_cleanup = declined;
  __cxa_get_globals()->uncaughtExceptions += 1;
  return &header->unwindHeader;
}

_Unwind_Exception *translate(EXCEPTION_RECORD *record, CONTEXT *context) {
  if (_se_translator_function translator = seh_translator) {
    EXCEPTION_POINTERS pointers = {record, context};
    try {
      translator(record->ExceptionCode, &pointers);
    } catch (...) {
      // Takes a reference of its own; the catch's end releases the one the
      // throw took, leaving exactly the reference a thrown object carries.
      if (void *thrown = __cxa_current_primary_exception())
        return hand_over(thrown);
    }
  }
  seh_exception *object = static_cast<seh_exception *>(
      __cxa_allocate_exception(sizeof(seh_exception)));
  object->record = *record;
  object->record.ExceptionRecord = nullptr;
  __cxa_exception *header = __cxa_init_primary_exception(
      object, const_cast<std::type_info *>(&typeid(seh_exception)),
      destroy_seh_exception);
  header->referenceCount = 1;
  return hand_over(object);
}

// throw; of the runtime's own object raises the structured exception again.
// __cxa_rethrow has marked the object rethrown; that is undone so that the
// catch block's end destroys it as usual, and the frames above are offered
// a fresh object for the exception raised again.
int rethrow_record(_Unwind_Exception *exc, EXCEPTION_RECORD *record) {
  if (!__isOurExceptionClass(exc) ||
      (__getExceptionClass(exc) & 0xFF) != 0)
    return 0;
  __cxa_exception *header = header_of(exc);
  if (header->exceptionType != &typeid(seh_exception))
    return 0;
  *record = static_cast<seh_exception *>(static_cast<void *>(header + 1))->record;
  if (header->handlerCount < 0) {
    header->handlerCount = -header->handlerCount;
    __cxa_get_globals()->uncaughtExceptions -= 1;
  }
  return 1;
}

} // namespace

// Called from the SEH personality thunk, so that the translator is in place
// before the first structured exception reaches a C++ frame, whether this
// runtime is a DLL or linked statically.
void registerSEHTranslator() {
  static bool registered = false;
  if (registered)
    return;
  _Unwind_SetSEHTranslator(translate, rethrow_record);
  registered = true;
}

} // namespace __cxxabiv1

extern "C" _se_translator_function __cdecl
_set_se_translator(_se_translator_function translator) {
  __cxxabiv1::registerSEHTranslator();
  _se_translator_function previous = __cxxabiv1::seh_translator;
  __cxxabiv1::seh_translator = translator;
  return previous;
}

#endif // __SEH__ && !__USING_SJLJ_EXCEPTIONS__ && _WIN32
