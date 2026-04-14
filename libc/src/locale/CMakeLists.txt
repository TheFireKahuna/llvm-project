set(locale_nls_deps "")
if(LIBC_TARGET_OS_IS_WINDOWS)
  list(APPEND locale_nls_deps libc.src.__support.OSUtil.windows.nls_locale)
endif()

add_object_library(
  locale
  SRCS
    locale.cpp
  HDRS
    locale.h
  DEPENDS
    libc.hdr.types.locale_t
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_entrypoint_object(
  localeconv
  SRCS
    localeconv.cpp
  HDRS
    localeconv.h
  DEPENDS
    libc.include.locale
    libc.src.__support.CPP.limits
    .locale
    ${locale_nls_deps}
  CXX_STANDARD
    20 # For designated initializers
)

if(LIBC_TARGET_OS_IS_WINDOWS)
  set(_newlocale_src windows/newlocale.cpp)
else()
  set(_newlocale_src newlocale.cpp)
endif()

add_entrypoint_object(
  newlocale
  SRCS
    ${_newlocale_src}
  HDRS
    newlocale.h
  DEPENDS
    libc.hdr.locale_macros
    libc.hdr.types.locale_t
    .locale
    ${locale_nls_deps}
)

if(LIBC_TARGET_OS_IS_WINDOWS)
  set(_duplocale_src windows/duplocale.cpp)
else()
  set(_duplocale_src duplocale.cpp)
endif()

add_entrypoint_object(
  duplocale
  SRCS
    ${_duplocale_src}
  HDRS
    duplocale.h
  DEPENDS
    libc.hdr.locale_macros
    libc.hdr.types.locale_t
    .locale
    ${locale_nls_deps}
)

if(LIBC_TARGET_OS_IS_WINDOWS)
  set(_setlocale_src windows/setlocale.cpp)
else()
  set(_setlocale_src setlocale.cpp)
endif()

add_entrypoint_object(
  setlocale
  SRCS
    ${_setlocale_src}
  HDRS
    setlocale.h
  DEPENDS
    libc.hdr.locale_macros
    libc.hdr.types.locale_t
    .locale
    ${locale_nls_deps}
)

add_entrypoint_object(
  uselocale
  SRCS
    uselocale.cpp
  HDRS
    uselocale.h
  DEPENDS
    libc.hdr.locale_macros
    libc.hdr.types.locale_t
    .locale
)

if(LIBC_TARGET_OS_IS_WINDOWS)
  set(_freelocale_src windows/freelocale.cpp)
else()
  set(_freelocale_src freelocale.cpp)
endif()

add_entrypoint_object(
  freelocale
  SRCS
    ${_freelocale_src}
  HDRS
    freelocale.h
  DEPENDS
    libc.hdr.types.locale_t
    .locale
)

add_entrypoint_object(
  nl_langinfo
  SRCS
    nl_langinfo.cpp
  HDRS
    nl_langinfo.h
  DEPENDS
    libc.hdr.types.nl_item
    libc.include.llvm-libc-macros.langinfo_macros
    .locale
    ${locale_nls_deps}
)

add_entrypoint_object(
  nl_langinfo_l
  SRCS
    nl_langinfo_l.cpp
  HDRS
    nl_langinfo_l.h
  DEPENDS
    libc.hdr.types.nl_item
    libc.hdr.types.locale_t
    libc.include.llvm-libc-macros.langinfo_macros
    .locale
    .nl_langinfo
    ${locale_nls_deps}
)
