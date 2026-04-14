# Windows startup code for LLVM libc
#
# This provides mainCRTStartup and DllMainCRTStartup entry points for
# Windows executables and DLLs built with LLVM libc.

function(add_startup_object name)
  cmake_parse_arguments(
    "ADD_STARTUP_OBJECT"
    "" # Option argument
    "OUTPUT_NAME;SRC"   # Single value arguments
    "DEPENDS;COMPILE_OPTIONS" # Multi value arguments
    ${ARGN}
  )

  get_fq_target_name(${name} fq_target_name)

  add_object_library(
    ${name}
    SRCS ${ADD_STARTUP_OBJECT_SRC}
    DEPENDS ${ADD_STARTUP_OBJECT_DEPENDS}
    COMPILE_OPTIONS ${ADD_STARTUP_OBJECT_COMPILE_OPTIONS}
  )
  if(ADD_STARTUP_OBJECT_OUTPUT_NAME)
    set(public_output_name ${ADD_STARTUP_OBJECT_OUTPUT_NAME})
  else()
    set(public_output_name ${name}.obj)
  endif()
  set_target_properties(
    ${fq_target_name}
    PROPERTIES
      OUTPUT_NAME ${public_output_name}
  )

  set(staged_output_dir ${LIBC_LIBRARY_DIR})
  set(staged_output ${staged_output_dir}/${public_output_name})
  if((WIN32_ITANIUM OR WIN32_NTPOSIX) AND LLVM_LIBRARY_OUTPUT_INTDIR)
    set(staged_output_dir ${LLVM_LIBRARY_OUTPUT_INTDIR})
    set(staged_output ${staged_output_dir}/${public_output_name})
  endif()
  add_custom_command(
    OUTPUT ${staged_output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${staged_output_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_OBJECTS:${fq_target_name}> ${staged_output}
    DEPENDS ${fq_target_name}
    COMMENT "Staging ${public_output_name} for ambient Windows Itanium startup"
    VERBATIM
  )
  add_custom_target(${name}_ambient DEPENDS ${staged_output})
  set_target_properties(
    ${fq_target_name}
    PROPERTIES
      LIBC_PUBLIC_OUTPUT ${staged_output}
      LIBC_PUBLIC_TARGET ${name}_ambient
  )
endfunction()

if(NOT (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_ARCHITECTURE}))
  message(STATUS "Skipping startup for target architecture ${LIBC_TARGET_ARCHITECTURE}")
  return()
endif()

add_subdirectory(${LIBC_TARGET_ARCHITECTURE})

# Internal libc initialization — compiled into the static archive and pulled
# into c.dll via WHOLEARCHIVE. Provides __libc_init() which crt_do_start.obj
# calls at EXE startup.
add_object_library(
  libc_init
  SRCS
    libc_init.cpp
  DEPENDS
    libc.config.app_h
    libc.hdr.stdint_proxy
    libc.src.__support.OSUtil.windows.fd_table
    libc.src.__support.threads.thread
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.stdlib.free
    libc.src.stdlib.malloc
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# EXE-side startup — calls __libc_init (from c.dll), walks CRT sections,
# calls main(), exits. No internal libc deps — uses only extern "C" imports.
add_startup_object(
  do_start
  OUTPUT_NAME
    crt_do_start.obj
  SRC
    do_start.cpp
  DEPENDS
    libc.startup.windows.libc_init
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Generic DLL entry point — still used by dll_crt.cpp (bundled into dllcrt.obj
# for user DLLs). c.dll now uses libc_dll_entry instead.
add_startup_object(
  dll_startup
  SRC
    dll_startup.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# c.dll dedicated entry point — uses explicit __libc_dll_init() instead of
# .CRT$XI* section walking. Only linked into c.dll, not user DLLs.
add_startup_object(
  libc_dll_entry
  SRC
    libc_dll_entry.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Bundled DLL CRT — installed as dllcrt.obj for DLLs that don't have libc
# (libunwind, libc++, user DLLs). Includes self-contained atexit and
# weak __cxa_thread_finalize so dll_startup.cpp's references resolve.
add_startup_object(
  dll_crt
  OUTPUT_NAME
    dllcrt.obj
  SRC
    dll_crt.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# PE infrastructure objects — required for any binary to link.
# These provide linker-required symbols (__security_cookie, _tls_used,
# _load_config_used, __guard_check_icall_fptr) that the PE loader and
# lld-link expect.

add_startup_object(
  security_cookie
  OUTPUT_NAME
    crt_gs.obj
  SRC
    security_cookie.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.bcryptprimitives
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  tls_directory
  OUTPUT_NAME
    crt_tls.obj
  SRC
    tls_directory.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  cfguard
  OUTPUT_NAME
    crt_cfg.obj
  SRC
    cfguard.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  load_config
  OUTPUT_NAME
    crt_loadcfg.obj
  SRC
    load_config.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  delayload
  OUTPUT_NAME
    crt_delayload.obj
  SRC
    delayload.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.windows_util
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# RTTI section sealing — seals .rdata$ti (dynamically-initialized typeinfo)
# as irrevocably read-only via SEC_NO_CHANGE after init ctors run.
add_startup_object(
  rtti_seal
  OUTPUT_NAME
    crt_rtti_seal.obj
  SRC
    rtti_seal.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_custom_target(libc-startup)
set(startup_components
  start do_start dll_startup libc_dll_entry dll_crt
  security_cookie tls_directory cfguard load_config delayload rtti_seal
)
foreach(target IN LISTS startup_components)
  if(TARGET libc.startup.windows.${target})
    set(fq_target_name libc.startup.windows.${target})
  elseif(TARGET libc.startup.windows.${LIBC_TARGET_ARCHITECTURE}.${target})
    set(fq_target_name libc.startup.windows.${LIBC_TARGET_ARCHITECTURE}.${target})
  else()
    continue()
  endif()
  get_target_property(public_output ${fq_target_name} LIBC_PUBLIC_OUTPUT)
  get_target_property(public_target ${fq_target_name} LIBC_PUBLIC_TARGET)
  add_dependencies(libc-startup ${public_target})
  install(FILES ${public_output}
          DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
          COMPONENT libc)
endforeach()
