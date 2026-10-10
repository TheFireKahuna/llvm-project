// A stand-in for the UCRT's new.h. With _MSC_EXTENSIONS it declares
// std::set_new_handler itself, using a macro only the Visual C++ runtime's
// crtdefs.h defines; otherwise it includes <new>.
#pragma once
#include <corecrt.h>
#include <vcruntime_new_debug.h>
#ifdef __cplusplus
#ifdef _MSC_EXTENSIONS
namespace std {
typedef void(__CRTDECL *new_handler)();
_CRTIMP2 new_handler __cdecl set_new_handler(new_handler) throw();
} // namespace std
#else
#include <new>
#endif
#endif
