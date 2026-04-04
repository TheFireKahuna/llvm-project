# Windows implementations of POSIX dlfcn functions via NT loader APIs.
#
# dlopen  -> LdrLoadDll
# dlclose -> LdrUnloadDll
# dlsym   -> LdrGetProcedureAddress (RTLD_DEFAULT searches all modules)
# dladdr  -> RtlPcToFileHeader + PEB loader walk + PE export table
# dlinfo  -> RTLD_DI_ORIGIN via PEB loader walk
# dlerror -> Thread-local NTSTATUS -> string table (pure ntdll)

add_entrypoint_object(
  dlopen
  SRCS
    dlopen.cpp
  HDRS
    ../dlopen.h
  DEPENDS
    libc.hdr.dlfcn_macros
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.dlfcn_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  dlclose
  SRCS
    dlclose.cpp
  HDRS
    ../dlclose.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.dlfcn_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  dlsym
  SRCS
    dlsym.cpp
  HDRS
    ../dlsym.h
  DEPENDS
    libc.hdr.dlfcn_macros
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.dlfcn_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  dlerror
  SRCS
    dlerror.cpp
  HDRS
    ../dlerror.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.dlfcn_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  dladdr
  SRCS
    dladdr.cpp
  HDRS
    ../dladdr.h
  DEPENDS
    libc.hdr.types.dl_info
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.dlfcn_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  dlinfo
  SRCS
    dlinfo.cpp
  HDRS
    ../dlinfo.h
  DEPENDS
    libc.hdr.dlfcn_macros
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.dlfcn_ops
    libc.src.__support.OSUtil.windows.ntdll
)
