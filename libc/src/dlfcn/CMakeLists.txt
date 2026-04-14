if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})

  add_entrypoint_object(
    dlclose
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.dlclose
  )

  add_entrypoint_object(
    dlerror
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.dlerror
  )

  add_entrypoint_object(
    dlopen
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.dlopen
  )

  add_entrypoint_object(
    dlsym
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.dlsym
  )

  add_entrypoint_object(
    dladdr
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.dladdr
  )

  add_entrypoint_object(
    dlinfo
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.dlinfo
  )
else()
  # Stub implementations for platforms without native dlfcn support.
  add_entrypoint_object(
    dlclose
    SRCS
      dlclose.cpp
    HDRS
      dlclose.h
  )

  add_entrypoint_object(
    dlerror
    SRCS
      dlerror.cpp
    HDRS
      dlerror.h
    DEPENDS
      libc.include.dlfcn
  )

  add_entrypoint_object(
    dlopen
    SRCS
      dlopen.cpp
    HDRS
      dlopen.h
    DEPENDS
      libc.include.dlfcn
  )

  add_entrypoint_object(
    dlsym
    SRCS
      dlsym.cpp
    HDRS
      dlsym.h
    DEPENDS
      libc.include.dlfcn
  )

  add_entrypoint_object(
    dlinfo
    SRCS
      dlinfo.cpp
    HDRS
      dlinfo.h
    DEPENDS
      libc.include.dlfcn
  )

  add_entrypoint_object(
    dladdr
    SRCS
      dladdr.cpp
    HDRS
      dladdr.h
    DEPENDS
      libc.include.dlfcn
      libc.hdr.types.dl_info
  )
endif()
