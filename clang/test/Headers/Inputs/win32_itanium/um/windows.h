// A stand-in for the Windows SDK's windows.h, with the declarations that
// delayimp.h uses.
#pragma once
#define WINAPI __stdcall
#define FACILITY_VISUALCPP 109
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_SEVERITY_ERROR 0xC0000000
typedef int BOOL;
typedef unsigned long DWORD;
typedef long HRESULT;
typedef const char *LPCSTR;
typedef struct HINSTANCE__ *HMODULE;
typedef long long(WINAPI *FARPROC)(void);
typedef struct _IMAGE_THUNK_DATA64 {
  union {
    unsigned long long Function;
  } u1;
} IMAGE_THUNK_DATA;
typedef struct _IMAGE_DELAYLOAD_DESCRIPTOR {
  DWORD Attributes;
  DWORD DllNameRVA;
  DWORD ModuleHandleRVA;
  DWORD ImportAddressTableRVA;
  DWORD ImportNameTableRVA;
  DWORD BoundImportAddressTableRVA;
  DWORD UnloadInformationTableRVA;
  DWORD TimeDateStamp;
} IMAGE_DELAYLOAD_DESCRIPTOR;
