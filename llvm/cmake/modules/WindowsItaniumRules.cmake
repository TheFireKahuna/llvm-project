# WindowsItaniumRules.cmake - CMake rules override for Windows Itanium
#
# Use via CMAKE_USER_MAKE_RULES_OVERRIDE before project():
#   set(CMAKE_USER_MAKE_RULES_OVERRIDE "/path/to/WindowsItaniumRules.cmake")
#
# CMake loads Windows-GNU.cmake for Clang (due to __GNUC__), generating MinGW
# flags that lld-link rejects. This module overrides with MSVC-style rules
# for lld-link compatibility.

# When loaded via CMAKE_USER_MAKE_RULES_OVERRIDE, must run for each language.
# When loaded via include(), guard against multiple inclusion.
if(NOT CMAKE_CURRENT_LIST_FILE STREQUAL CMAKE_USER_MAKE_RULES_OVERRIDE)
  include_guard(GLOBAL)
endif()

# Propagate Windows Itanium settings to try_compile() calls.
# Without this, feature detection tests use default Windows-GNU rules.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  CMAKE_USER_MAKE_RULES_OVERRIDE
  CMAKE_C_COMPILER_TARGET
  CMAKE_CXX_COMPILER_TARGET
)

# MSVC/clang-cl will use CMake's Clang-Windows.cmake platform file, exit here.
# The remaining CMake configuration overrides CMake's default Windows-GNU.cmake
# platform file, until an official Windows-Itanium.cmake alternative is upstreamed.
if(NOT CMAKE_C_COMPILER_TARGET MATCHES "windows-(itanium|ntposix)" AND
   NOT CMAKE_CXX_COMPILER_TARGET MATCHES "windows-(itanium|ntposix)" AND
   NOT LLVM_RUNTIMES_TARGET MATCHES "windows-(itanium|ntposix)" AND
   NOT LLVM_HOST_TRIPLE MATCHES "windows-(itanium|ntposix)")
  return()
endif()

# Windows Itanium uses libc++. Tell CMake so it finds libc++.modules.json for import std.
set(CMAKE_CXX_STANDARD_LIBRARY "libc++" CACHE STRING "C++ standard library")

# CMake only sets CMAKE_<LANG>_COMPILER_ARCHITECTURE_ID for windows-msvc targets.
# Set it for windows-itanium so downstream code (e.g., lib.exe /machine:) works.
set(_arch "")
foreach(_triple IN ITEMS
    "${CMAKE_C_COMPILER_TARGET}"
    "${CMAKE_CXX_COMPILER_TARGET}"
    "${LLVM_RUNTIMES_TARGET}")
  if(_triple MATCHES "^([^-]+)-.*windows-(itanium|ntposix)")
    set(_arch "${CMAKE_MATCH_1}")
    break()
  endif()
endforeach()

if(_arch MATCHES "^x86_64$|^amd64$")
  set(_arch_id "x64")
elseif(_arch MATCHES "^i[3-6]86$")
  set(_arch_id "X86")
elseif(_arch MATCHES "^aarch64$|^arm64")
  set(_arch_id "ARM64")
elseif(_arch MATCHES "^arm")
  set(_arch_id "ARM")
else()
  set(_arch_id "")
endif()

if(_arch_id AND NOT CMAKE_C_COMPILER_ARCHITECTURE_ID)
  set(CMAKE_C_COMPILER_ARCHITECTURE_ID "${_arch_id}")
endif()
if(_arch_id AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
  set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "${_arch_id}")
endif()

unset(_triple)
unset(_arch)
unset(_arch_id)

# Disable MinGW Mode enabled by Windows-GNU
set(MINGW FALSE)

# Match Windows platform behavior: VERSION/SOVERSION set image metadata, but
# should not produce versioned DLL/import-library filenames or symlink graphs.
set(CMAKE_PLATFORM_NO_VERSIONED_SONAME 1)
set(CMAKE_PLATFORM_HAS_INSTALLNAME 0)

# Library Naming - Match Windows-Clang.cmake __windows_compiler_clang_gnu
set(CMAKE_IMPORT_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_MODULE_PREFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_EXECUTABLE_SUFFIX ".exe")
set(CMAKE_IMPORT_LIBRARY_SUFFIX ".lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".dll")
set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")

set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll.a" ".a" ".lib")

# Library Path/Link Flags
set(CMAKE_LIBRARY_PATH_FLAG "-L")
set(CMAKE_LINK_LIBRARY_FLAG "-l")
set(CMAKE_DL_LIBS "")
set(CMAKE_LINK_LIBRARY_SUFFIX "")

# Export all symbols support
set(CMAKE_SUPPORT_WINDOWS_EXPORT_ALL_SYMBOLS 1)

# Linker Configuration - Match Windows-Clang.cmake __windows_compiler_clang_gnu
set(CMAKE_GNULD_IMAGE_VERSION "")

# Per-language settings via macro (matches Windows-Clang.cmake pattern)
macro(__windows_itanium_compiler lang)
  # Dependency file generation
  if(NOT "${lang}" STREQUAL "ASM")
    set(CMAKE_DEPFILE_FLAGS_${lang} "-MD -MT <DEP_TARGET> -MF <DEP_FILE>")
  endif()

  # Sysroot support
  set(CMAKE_${lang}_COMPILE_OPTIONS_SYSROOT "--sysroot=")

  # Linker wrapper flags
  set(CMAKE_${lang}_LINKER_WRAPPER_FLAG "-Xlinker" " ")
  set(CMAKE_${lang}_LINKER_WRAPPER_FLAG_SEP)

  # Link mode - use compiler driver, not direct linker invocation
  set(CMAKE_${lang}_LINK_MODE DRIVER)

  # Manifest and warning flags
  set(CMAKE_${lang}_LINKER_MANIFEST_FLAG " -Xlinker /MANIFESTINPUT:")
  set(CMAKE_${lang}_COMPILE_OPTIONS_WARNING_AS_ERROR "-Werror")

  # Verbose link flag
  set(CMAKE_${lang}_VERBOSE_LINK_FLAG "-v")

  # No PIC/PIE on Windows
  set(CMAKE_${lang}_COMPILE_OPTIONS_PIC "")
  set(CMAKE_${lang}_COMPILE_OPTIONS_PIE "")
  set(_CMAKE_${lang}_PIE_MAY_BE_SUPPORTED_BY_LINKER NO)
  set(CMAKE_${lang}_LINK_OPTIONS_PIE "")
  set(CMAKE_${lang}_LINK_OPTIONS_NO_PIE "")
  set(CMAKE_SHARED_LIBRARY_${lang}_FLAGS "")
  set(CMAKE_SHARED_LIBRARY_CREATE_${lang}_FLAGS "-shared")
  set(CMAKE_${lang}_SHARED_LIBRARY_COMPILE_DEFINITIONS "_WINDLL")

  # Linker selection - lld-link required for auto-import
  set(CMAKE_${lang}_USING_LINKER_DEFAULT "-fuse-ld=lld-link")
  set(CMAKE_${lang}_USING_LINKER_LLD "-fuse-ld=lld-link")
  # Warn if user tries to use link.exe (lacks auto-import support)
  set(CMAKE_${lang}_USING_LINKER_SYSTEM "-fuse-ld=lld-link")
  set(CMAKE_${lang}_USING_LINKER_MSVC "-fuse-ld=lld-link")

  # Response file support
  set(CMAKE_${lang}_USE_RESPONSE_FILE_FOR_OBJECTS 1)
  set(CMAKE_${lang}_USE_RESPONSE_FILE_FOR_LIBRARIES 1)
  set(CMAKE_${lang}_USE_RESPONSE_FILE_FOR_INCLUDES 1)

  # LTO/IPO support
  if(CMAKE_${lang}_COMPILER_VERSION VERSION_GREATER_EQUAL 3.9)
    set(CMAKE_${lang}_COMPILE_OPTIONS_IPO "-flto=thin")
  else()
    set(CMAKE_${lang}_COMPILE_OPTIONS_IPO "-flto")
  endif()
  set(_CMAKE_${lang}_IPO_SUPPORTED_BY_CMAKE YES)
  set(_CMAKE_${lang}_IPO_MAY_BE_SUPPORTED_BY_COMPILER YES)
  set(CMAKE_${lang}_ARCHIVE_CREATE_IPO "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_APPEND_IPO "<CMAKE_AR> q <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_FINISH_IPO "<CMAKE_RANLIB> <TARGET>")

  # Archive/static library rules
  set(CMAKE_${lang}_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_APPEND "<CMAKE_AR> q <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_FINISH "<CMAKE_RANLIB> <TARGET>")

  # Link commands - Windows Itanium uses Clang driver which invokes lld-link
  # Note: Unlike Windows-Clang.cmake, we don't use -nostartfiles/-nostdlib
  # because Windows Itanium links against UCRT and uses its own CRT startup.
  set(CMAKE_${lang}_CREATE_SHARED_LIBRARY
    "<CMAKE_${lang}_COMPILER> <CMAKE_SHARED_LIBRARY_${lang}_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> -o <TARGET> -Xlinker /MANIFEST:EMBED -Xlinker /implib:<TARGET_IMPLIB> -Xlinker /pdb:<TARGET_PDB> -Xlinker /version:<TARGET_VERSION_MAJOR>.<TARGET_VERSION_MINOR> <OBJECTS> <LINK_LIBRARIES> <MANIFESTS>")
  set(CMAKE_${lang}_CREATE_SHARED_MODULE ${CMAKE_${lang}_CREATE_SHARED_LIBRARY})
  set(CMAKE_${lang}_LINK_EXECUTABLE
    "<CMAKE_${lang}_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Xlinker /MANIFEST:EMBED -Xlinker /implib:<TARGET_IMPLIB> -Xlinker /pdb:<TARGET_PDB> -Xlinker /version:<TARGET_VERSION_MAJOR>.<TARGET_VERSION_MINOR> <LINK_LIBRARIES> <MANIFESTS>")

  # Subsystem flags
  set(CMAKE_${lang}_CREATE_WIN32_EXE "-Xlinker /subsystem:windows")
  set(CMAKE_${lang}_CREATE_CONSOLE_EXE "-Xlinker /subsystem:console")

  # DEF file flag
  set(CMAKE_${lang}_LINK_DEF_FILE_FLAG "-Xlinker /DEF:")

  # PDB support
  set(CMAKE_${lang}_LINKER_SUPPORTS_PDB ON)

  # SONAME not used on Windows
  set(CMAKE_SHARED_LIBRARY_SONAME_${lang}_FLAG "")

  # MSVC runtime library options (Windows Itanium always uses dynamic UCRT)
  if(NOT "${lang}" STREQUAL "ASM")
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreaded         -Xclang -flto-visibility-public-std -D_MT)
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL      -D_DLL -D_MT)
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebug    -D_DEBUG -Xclang -flto-visibility-public-std -D_MT)
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebugDLL -D_DEBUG -D_DLL -D_MT)

    # MSVC runtime checks (Clang ignores these but we define empty values)
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_CHECKS_PossibleDataLoss      "")
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_CHECKS_StackFrameErrorCheck  "")
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_CHECKS_UninitializedVariable "")
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_RUNTIME_CHECKS_RTCsu                 "")

    # Debug information format
    set(CMAKE_${lang}_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_Embedded -g -Xclang -gcodeview)
  endif()

  # System include flag
  set(CMAKE_INCLUDE_SYSTEM_FLAG_${lang} "-isystem ")

  # Standard libraries - empty, driver adds -defaultlib:kernel32 etc. (WindowsItanium.cpp:549-561)
  set(CMAKE_${lang}_STANDARD_LIBRARIES_INIT "")
endmacro()

# Apply settings for C and CXX
__windows_itanium_compiler(C)
__windows_itanium_compiler(CXX)

# Set global LINK_DEF_FILE_FLAG
set(CMAKE_LINK_DEF_FILE_FLAG "${CMAKE_C_LINK_DEF_FILE_FLAG}")

# PCH support
set(CMAKE_PCH_EXTENSION .pch)
set(CMAKE_PCH_PROLOGUE "#pragma clang system_header")
set(CMAKE_C_COMPILE_OPTIONS_USE_PCH -Xclang -include-pch -Xclang <PCH_FILE> -Xclang -include -Xclang <PCH_HEADER>)
set(CMAKE_C_COMPILE_OPTIONS_CREATE_PCH -Xclang -emit-pch -Xclang -include -Xclang <PCH_HEADER> -x c-header)
set(CMAKE_CXX_COMPILE_OPTIONS_USE_PCH -Xclang -include-pch -Xclang <PCH_FILE> -Xclang -include -Xclang <PCH_HEADER>)
set(CMAKE_CXX_COMPILE_OPTIONS_CREATE_PCH -Xclang -emit-pch -Xclang -include -Xclang <PCH_HEADER> -x c++-header)

# Build type flag initialization
if(CMAKE_MSVC_RUNTIME_LIBRARY_DEFAULT)
  set(_RTL_FLAGS "")
  set(_RTL_FLAGS_DEBUG "")
else()
  set(_RTL_FLAGS_DEBUG " -D_DEBUG -D_DLL -D_MT")
  set(_RTL_FLAGS " -D_DLL -D_MT")
endif()

if(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT_DEFAULT)
  set(_DBG_FLAGS "")
else()
  set(_DBG_FLAGS " -g -Xclang -gcodeview")
endif()

string(APPEND CMAKE_C_FLAGS_DEBUG_INIT " -O0${_DBG_FLAGS}${_RTL_FLAGS_DEBUG}")
string(APPEND CMAKE_C_FLAGS_MINSIZEREL_INIT " -Os -DNDEBUG${_RTL_FLAGS}")
string(APPEND CMAKE_C_FLAGS_RELEASE_INIT " -O3 -DNDEBUG${_RTL_FLAGS}")
string(APPEND CMAKE_C_FLAGS_RELWITHDEBINFO_INIT " -O2 -DNDEBUG${_DBG_FLAGS}${_RTL_FLAGS}")

string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " -O0${_DBG_FLAGS}${_RTL_FLAGS_DEBUG}")
string(APPEND CMAKE_CXX_FLAGS_MINSIZEREL_INIT " -Os -DNDEBUG${_RTL_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS_RELEASE_INIT " -O3 -DNDEBUG${_RTL_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT " -O2 -DNDEBUG${_DBG_FLAGS}${_RTL_FLAGS}")

unset(_DBG_FLAGS)
unset(_RTL_FLAGS)
unset(_RTL_FLAGS_DEBUG)

# LLVM Toolchain Programs
# Prefer LLVM tools adjacent to the selected compiler to avoid VC SDK tools
# (lib.exe, mt.exe, rc.exe), but preserve explicitly configured archive tools.
if(CMAKE_C_COMPILER)
  get_filename_component(_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)

  macro(_find_llvm_tool var name)
    find_program(${var} NAMES ${name} HINTS "${_compiler_dir}" NO_DEFAULT_PATH)
    if(NOT ${var})
      find_program(${var} NAMES ${name})
    endif()
  endmacro()

  if(NOT CMAKE_AR)
    _find_llvm_tool(_llvm_ar llvm-ar)
    if(_llvm_ar)
      set(CMAKE_AR "${_llvm_ar}")
    endif()
  endif()

  if(NOT CMAKE_RANLIB)
    _find_llvm_tool(_llvm_ranlib llvm-ranlib)
    if(_llvm_ranlib)
      set(CMAKE_RANLIB "${_llvm_ranlib}")
    endif()
  endif()

  _find_llvm_tool(_llvm_rc llvm-rc)
  if(_llvm_rc)
    set(CMAKE_RC_COMPILER "${_llvm_rc}")
    set(CMAKE_RC_COMPILER_INIT "${_llvm_rc}")
  endif()

  _find_llvm_tool(_llvm_mt llvm-mt)
  if(_llvm_mt)
    set(CMAKE_MT "${_llvm_mt}")
  endif()

  _find_llvm_tool(_llvm_ml llvm-ml64)
  if(_llvm_ml)
    set(CMAKE_ASM_MASM_COMPILER "${_llvm_ml}")
  endif()

  unset(_llvm_ar)
  unset(_llvm_ranlib)
  unset(_llvm_rc)
  unset(_llvm_mt)
  unset(_llvm_ml)
  unset(_compiler_dir)
endif()

# RC compilation with llvm-rc.
# llvm-rc doesn't support -MD/-MF dependency flags directly. Use cmake_llvm_rc
# to preprocess with the C compiler (which handles deps) then compile with llvm-rc.
# Matches Windows-Clang.cmake's __enable_llvm_rc_preprocessing approach.
if(CMAKE_RC_COMPILER MATCHES "llvm-rc")
  set(CMAKE_DEPFILE_FLAGS_RC "-MD -MF <DEP_FILE>")
  set(CMAKE_RC_COMPILE_OBJECT
    "<CMAKE_COMMAND> -E cmake_llvm_rc <SOURCE> <OBJECT>.pp <CMAKE_C_COMPILER> <DEFINES> -DRC_INVOKED <INCLUDES> <FLAGS> -x c -E -- <SOURCE> ++ <CMAKE_RC_COMPILER> <DEFINES> -I <SOURCE_DIR> <INCLUDES> <FLAGS> /fo <OBJECT> <OBJECT>.pp")
  if(CMAKE_GENERATOR MATCHES "Ninja")
    set(CMAKE_NINJA_CMCLDEPS_RC 0)
    set(CMAKE_NINJA_DEP_TYPE_RC gcc)
  endif()
else()
  # Fallback for non-llvm-rc (shouldn't happen, but be safe)
  set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /fo <OBJECT> <SOURCE>")
endif()

# Note: Don't call enable_language(RC) here - it causes recursion when loaded
# via CMAKE_USER_MAKE_RULES_OVERRIDE. Projects should enable RC if needed.
