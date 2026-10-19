/*===---- delayimp.h - Delay-load helper interface -------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The interface of the delay-load helper that the linker's /delayload thunks
 * call, which the Visual C++ tools declare rather than the Windows SDK. On
 * Windows Itanium the start-up library implements it. Through it a program
 * installs a hook that sees or replaces each stage of resolving an import, or
 * is asked when one fails; resolves every import of a library at once; and
 * unloads a library that the image was linked to unload, with /delay:unload. */

#ifndef __CLANG_DELAYIMP_H
#define __CLANG_DELAYIMP_H

#include <windows.h>

#define _DELAY_IMP_VER 2

#ifndef ExternC
#ifdef __cplusplus
#define ExternC extern "C"
#else
#define ExternC extern
#endif
#endif

typedef IMAGE_THUNK_DATA *PImgThunkData;
typedef const IMAGE_THUNK_DATA *PCImgThunkData;
typedef DWORD RVA;

/* A library's delay-load descriptor, laid out as the SDK's
 * IMAGE_DELAYLOAD_DESCRIPTOR. */
typedef struct ImgDelayDescr {
  DWORD grAttrs;
  RVA rvaDLLName;
  RVA rvaHmod;
  RVA rvaIAT;
  RVA rvaINT;
  RVA rvaBoundIAT;
  RVA rvaUnloadIAT;
  DWORD dwTimeStamp;
} ImgDelayDescr, *PImgDelayDescr;

typedef const ImgDelayDescr *PCImgDelayDescr;

/* The helper accepts only a descriptor that holds relative addresses. */
enum DLAttr { dlattrRva = 0x1 };

/* The stage a hook is called at. A nonnull answer to the notify hook replaces
 * the stage: at the start, the address the call goes to, which is not stored;
 * before the library is loaded, the library; before the procedure is looked
 * up, its address. An answer to the failure hook is the library or procedure
 * that could not be found. */
enum {
  dliStartProcessing,
  dliNoteStartProcessing = dliStartProcessing,
  dliNotePreLoadLibrary,
  dliNotePreGetProcAddress,
  dliFailLoadLib,
  dliFailGetProc,
  dliNoteEndProcessing
};

typedef struct DelayLoadProc {
  BOOL fImportByName;
  union {
    LPCSTR szProcName;
    DWORD dwOrdinal;
  };
} DelayLoadProc;

/* The import being resolved, as a hook and the failure exception see it. */
typedef struct DelayLoadInfo {
  DWORD cb;
  PCImgDelayDescr pidd;
  FARPROC *ppfn;
  LPCSTR szDll;
  DelayLoadProc dlp;
  HMODULE hmodCur;
  FARPROC pfnCur;
  DWORD dwLastError;
} DelayLoadInfo, *PDelayLoadInfo;

typedef FARPROC(WINAPI *PfnDliHook)(unsigned dliNotify, PDelayLoadInfo pdli);

/* Restores the library's imports to their thunks and frees it. FALSE when the
 * image does not delay-load it, has not loaded it, or was not linked with
 * /delay:unload. */
ExternC BOOL WINAPI __FUnloadDelayLoadedDLL2(LPCSTR szDll);

/* Resolves every import of the library now, reporting a failure as a call
 * through the import would. */
ExternC HRESULT WINAPI __HrLoadAllImportsForDll(LPCSTR szDll);

/* A failure that no hook answers raises the exception
 * VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) for a library, or
 * ERROR_PROC_NOT_FOUND for a procedure, whose one argument points to the
 * DelayLoadInfo. */
#define VcppException(sev, err) ((sev) | (FACILITY_VISUALCPP << 16) | err)

/* The hooks a program defines. They are constant, so that a stray write cannot
 * redirect them; a program that must change a hook while it runs defines
 * DELAYIMP_INSECURE_WRITABLE_HOOKS and a writable definition. */
#ifdef DELAYIMP_INSECURE_WRITABLE_HOOKS
ExternC PfnDliHook __pfnDliNotifyHook2;
ExternC PfnDliHook __pfnDliFailureHook2;
#else
ExternC const PfnDliHook __pfnDliNotifyHook2;
ExternC const PfnDliHook __pfnDliFailureHook2;
#endif

/* Whether the helper makes the whole section that holds the protected table
 * writable while it writes, rather than the pages it writes. It is accepted
 * and has no effect: the helper reopens only the pages it writes, as the
 * loader does, and a wider window would only leave more of the table open. */
#ifdef DELAYIMP_WRITABLE_DLOAD_SECTION_PROTECTION_POLICY
ExternC BOOL __bChangeProtectionOfWholeDloadSection;
#else
ExternC const BOOL __bChangeProtectionOfWholeDloadSection;
#endif

#endif /* __CLANG_DELAYIMP_H */
