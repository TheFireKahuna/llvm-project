# Optional lifetime integration; libc++ retains its native Windows backend.
include_guard(GLOBAL)
set(_LIBCPP_HAS_SHARED_THREAD_LOCAL_DATA ON)
set(LIBCXX_HAS_WIN32_THREAD_API ON CACHE BOOL "Use the native Windows backend" FORCE)

function(wincrt_configure_libcxx)
  if(NOT TARGET cxx_shared OR LIBCXX_ENABLE_STATIC OR LIBCXXABI_ENABLE_SHARED OR
     NOT CMAKE_CXX_COMPILER_TARGET MATCHES "windows-itanium")
    message(FATAL_ERROR "wincrt lifetime integration requires Windows Itanium shared libc++ with static libc++abi")
  endif()
  set(wincrt_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  set(wincrt_lifetime_sources "${wincrt_dir}/cxa_thread_atexit.cpp"
                              "${wincrt_dir}/cxa_atexit.cpp")
  target_sources(cxx_shared PRIVATE ${wincrt_lifetime_sources})
  set_source_files_properties(${wincrt_lifetime_sources}
    TARGET_DIRECTORY cxx_shared PROPERTIES
    COMPILE_DEFINITIONS WINCRT_SHARED_CXX_RUNTIME=1
    COMPILE_OPTIONS "-mguard=cf")
  # The driver turns this into the guard modes the target supports.
  target_link_options(cxx_shared PRIVATE "-mguard=cf")
  if(CMAKE_CXX_COMPILER_TARGET MATCHES "^i.86-")
    target_link_options(cxx_shared PRIVATE "LINKER:/include:___cxa_thread_atexit_impl" "LINKER:/include:___wincrt_shared_tls_callback")
  else()
    target_link_options(cxx_shared PRIVATE "LINKER:/include:__cxa_thread_atexit_impl" "LINKER:/include:__wincrt_shared_tls_callback")
  endif()
endfunction()
cmake_language(DEFER CALL wincrt_configure_libcxx)
