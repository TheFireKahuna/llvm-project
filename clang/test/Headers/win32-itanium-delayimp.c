// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=CONST
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -emit-llvm -o - -x c++ %s | FileCheck %s --check-prefix=CONST
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -emit-llvm -o - -x c++ %s | FileCheck %s --check-prefix=CONST
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -DDELAYIMP_INSECURE_WRITABLE_HOOKS \
// RUN:     -DDELAYIMP_WRITABLE_DLOAD_SECTION_PROTECTION_POLICY \
// RUN:     -emit-llvm -o - -x c++ %s | FileCheck %s --check-prefix=WRITABLE

// delayimp.h declares the delay-load helper's interface with Visual C++'s
// layout and names. A program's hooks and its choice of section-wide
// protection are constant, with C linkage, unless it asks for writable ones.

#include <delayimp.h>

#ifdef __cplusplus
#define _Static_assert static_assert
#endif

_Static_assert(sizeof(ImgDelayDescr) == sizeof(IMAGE_DELAYLOAD_DESCRIPTOR), "");
_Static_assert(__builtin_offsetof(ImgDelayDescr, rvaUnloadIAT) == 24, "");
_Static_assert(__builtin_offsetof(DelayLoadInfo, pidd) == 8, "");
_Static_assert(__builtin_offsetof(DelayLoadInfo, dlp) == 32, "");
_Static_assert(__builtin_offsetof(DelayLoadInfo, dlp.szProcName) == 40, "");
_Static_assert(__builtin_offsetof(DelayLoadInfo, hmodCur) == 48, "");
_Static_assert(__builtin_offsetof(DelayLoadInfo, dwLastError) == 64, "");
_Static_assert(sizeof(DelayLoadInfo) == 72, "");
_Static_assert(dlattrRva == 1, "");
_Static_assert(dliNoteStartProcessing == 0 && dliNoteEndProcessing == 5, "");
_Static_assert(VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) ==
                   0xC06D007F,
               "");

static FARPROC WINAPI hook(unsigned Reason, PDelayLoadInfo Info) {
  return Reason == dliFailGetProc ? Info->pfnCur : 0;
}

// CONST: @__pfnDliNotifyHook2 = dso_local constant ptr @{{.*}}hook
// CONST: @__pfnDliFailureHook2 = dso_local constant ptr @{{.*}}hook
// WRITABLE: @__pfnDliNotifyHook2 = dso_local global ptr @{{.*}}hook
// WRITABLE: @__pfnDliFailureHook2 = dso_local global ptr @{{.*}}hook
#ifdef DELAYIMP_INSECURE_WRITABLE_HOOKS
PfnDliHook __pfnDliNotifyHook2 = hook;
PfnDliHook __pfnDliFailureHook2 = hook;
#else
const PfnDliHook __pfnDliNotifyHook2 = hook;
const PfnDliHook __pfnDliFailureHook2 = hook;
#endif

// CONST: @__bChangeProtectionOfWholeDloadSection = dso_local constant i32 1
// WRITABLE: @__bChangeProtectionOfWholeDloadSection = dso_local global i32 1
#ifdef DELAYIMP_WRITABLE_DLOAD_SECTION_PROTECTION_POLICY
BOOL __bChangeProtectionOfWholeDloadSection = 1;
#else
const BOOL __bChangeProtectionOfWholeDloadSection = 1;
#endif

// CONST: declare {{.*}}i32 @__FUnloadDelayLoadedDLL2(ptr
// CONST: declare {{.*}}i32 @__HrLoadAllImportsForDll(ptr
BOOL unload(void) { return __FUnloadDelayLoadedDLL2("a.dll"); }
HRESULT loadAll(void) { return __HrLoadAllImportsForDll("a.dll"); }
