//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Exceptions that are not this runtime's own, in C++ handlers on Windows
//  Itanium. While the system searches for a handler, libunwind asks a frame's
//  personality routine about such an exception with an object that stands for
//  its EXCEPTION_RECORD. Another C++ runtime's exception is a foreign
//  exception, which catch (...) takes in any frame. A structured exception is
//  offered to a frame compiled for asynchronous exceptions, as the object a
//  translator installed with _set_se_translator throws for it or as one of a
//  type no handler can name, which is raised again as the structured
//  exception by throw; and std::rethrow_exception.
//
//===----------------------------------------------------------------------===//

#include "cxxabi.h"

#if defined(_WIN32_ITANIUM)

#  include "cxa_exception.h"
#  include <exception>
#  include <stdlib.h>
#  include <string.h>
#  include <typeinfo>
#  include <unwind.h>
#  include <windows.h>

extern "C" {
typedef void(__cdecl* _se_translator_function)(unsigned int, struct _EXCEPTION_POINTERS*);
_LIBCXXABI_FUNC_VIS _se_translator_function __cdecl _set_se_translator(_se_translator_function);
}

namespace __cxxabiv1 {
namespace {

thread_local _se_translator_function translator = nullptr;

// What catch (...) receives for a structured exception that no translator
// turned into an object of its own.
struct structured_exception {
  EXCEPTION_RECORD record;
};

const DWORD msvc_cxx_exception = 0xE06D7363;
const DWORD gcc_cxx_exception  = 0x20474343;         // STATUS_GCC_THROW
const uint64_t foreign_class   = 0x4D535643432B2B00; // MSVCC++\0

const EXCEPTION_RECORD* record_of(const _Unwind_Exception* asked) {
  return reinterpret_cast<const EXCEPTION_RECORD*>(asked->private_[0]);
}

int translator_raised(DWORD code) {
  // A C++ exception that the translator throws goes on to its caller. A
  // structured exception that leaves it ends the program.
  if (code != msvc_cxx_exception && code != gcc_cxx_exception)
    std::terminate();
  return EXCEPTION_CONTINUE_SEARCH;
}

// A function has a try block or a __try block, not both.
void call_translator(_se_translator_function function, EXCEPTION_POINTERS* pointers) {
  __try {
    function(pointers->ExceptionRecord->ExceptionCode, pointers);
  } __except (translator_raised(GetExceptionCode())) {
  }
}

void delete_foreign(_Unwind_Reason_Code, _Unwind_Exception* exception) { free(exception); }

// The object of an MSVC C++ exception lives in the frame of its throw, which
// the unwind to a handler discards. Destroys it as vcruntime does when a
// handler of its own is left: by the unwind function of the record's
// ThrowInfo, an address relative to the image the record names. That is
// another runtime's function, of no KCFI type, and it may not throw.
__attribute__((no_sanitize("kcfi"))) void destroy_msvc_object(const EXCEPTION_RECORD* record) {
  struct throw_info {
    unsigned attributes;
    int unwind, forward_compat, catchable_types;
  };
  if (record->ExceptionCode != msvc_cxx_exception || record->NumberParameters != 4)
    return;
  const throw_info* info = reinterpret_cast<const throw_info*>(record->ExceptionInformation[2]);
  if (!info || !info->unwind)
    return;
  __try {
    reinterpret_cast<void (*)(void*)>(record->ExceptionInformation[3] + info->unwind)(
        reinterpret_cast<void*>(record->ExceptionInformation[1]));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    std::terminate();
  }
}

} // namespace

bool __seh_is_structured(const _Unwind_Exception* asked) {
  return asked->exception_class == _UNWIND_SEH_EXCEPTION_CLASS && record_of(asked)->ExceptionCode != msvc_cxx_exception;
}

// A frame is offered every exception but a breakpoint, which is the debugger's.
bool __seh_is_offered(const _Unwind_Exception* asked) {
  return record_of(asked)->ExceptionCode != static_cast<DWORD>(STATUS_BREAKPOINT);
}

// Returns the C++ exception that the handlers of a frame are matched against
// for the structured exception libunwind asks about: what the thread's
// translator throws for it, or else an object that holds its record. The
// translator runs in the search phase, with nothing unwound.
_Unwind_Exception* __seh_translate(_Unwind_Exception* asked) {
  EXCEPTION_POINTERS pointers = {
      reinterpret_cast<EXCEPTION_RECORD*>(asked->private_[0]), reinterpret_cast<CONTEXT*>(asked->private_[1])};
  void* thrown = nullptr;
  if (_se_translator_function function = translator) {
    try {
      call_translator(function, &pointers);
    } catch (...) {
      // A reference of its own: the end of this handler drops the throw's.
      thrown = __cxa_current_primary_exception();
    }
  }
  if (!thrown) {
    thrown                         = __cxa_allocate_exception(sizeof(structured_exception));
    structured_exception* object   = static_cast<structured_exception*>(thrown);
    object->record                 = *pointers.ExceptionRecord;
    object->record.ExceptionRecord = nullptr;
    __cxa_init_primary_exception(thrown, const_cast<std::type_info*>(&typeid(structured_exception)), nullptr)
        ->referenceCount = 1;
  }
  // The object is in flight, as one that __cxa_throw raised is.
  __cxa_get_globals()->uncaughtExceptions += 1;
  _Unwind_Exception* exception = &(static_cast<__cxa_exception*>(thrown) - 1)->unwindHeader;
  asked->private_[2]           = reinterpret_cast<uintptr_t>(exception);
  return exception;
}

// Ends a frame's search about the exception libunwind asked with \p asked.
// Returns the object the landing pad of a frame that takes it receives:
// __seh_translate's, or a foreign exception for another C++ runtime's, whose
// own object does not outlive the search. A frame that declines the object of
// a structured exception destroys it; the search goes on with the structured
// exception itself.
_Unwind_Exception* __seh_search_result(_Unwind_Exception* asked, bool taken) {
  _Unwind_Exception* exception = reinterpret_cast<_Unwind_Exception*>(asked->private_[2]);
  if (!taken) {
    if (exception) {
      __cxa_get_globals()->uncaughtExceptions -= 1;
      __cxa_decrement_exception_refcount(exception + 1);
    }
    return nullptr;
  }
  if (!exception) {
    destroy_msvc_object(record_of(asked));
    exception = static_cast<_Unwind_Exception*>(calloc(1, sizeof(_Unwind_Exception)));
    if (!exception)
      std::terminate();
    exception->exception_class   = foreign_class;
    exception->exception_cleanup = delete_foreign;
    asked->private_[2]           = reinterpret_cast<uintptr_t>(exception);
  }
  return exception;
}

// Raises the structured exception again if \p header is that of an object
// that stands for one: a filter or a handler further out sees the code and
// the parameters it was first raised with. The handler the rethrow leaves
// destroys the object.
void __seh_rethrow(const __cxa_exception* header) {
  if (header->exceptionType != &typeid(structured_exception))
    return;
  EXCEPTION_RECORD record = static_cast<const structured_exception*>(static_cast<const void*>(header + 1))->record;
  RaiseException(record.ExceptionCode,
                 record.ExceptionFlags & EXCEPTION_NONCONTINUABLE,
                 record.NumberParameters,
                 record.ExceptionInformation);
  // A filter had the exception continue. A throw does not return.
  std::terminate();
}

} // namespace __cxxabiv1

extern "C" _se_translator_function __cdecl _set_se_translator(_se_translator_function function) {
  _se_translator_function previous = __cxxabiv1::translator;
  __cxxabiv1::translator           = function;
  return previous;
}

#endif // defined(_WIN32_ITANIUM)
