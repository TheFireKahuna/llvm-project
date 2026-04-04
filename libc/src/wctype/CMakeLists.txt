set(wctype_basic_deps
    libc.src.__support.wctype_utils
    libc.hdr.types.wint_t)

set(wctype_locale_basic_deps
    ${wctype_basic_deps}
    libc.hdr.types.locale_t)

set(wctype_classification_deps
    libc.src.__support.wctype.wctype_classification_utils
    libc.hdr.types.wint_t)

set(wctype_locale_classification_deps
    ${wctype_classification_deps}
    libc.hdr.types.locale_t)

set(wctype_descriptor_deps
    libc.src.__support.wctype_utils
    libc.src.__support.wctype.wctype_descriptors
    libc.hdr.types.wctype_t
    libc.hdr.types.wint_t)

set(wctype_locale_descriptor_deps
    ${wctype_descriptor_deps}
    libc.hdr.types.locale_t)

set(wctype_transform_deps
    libc.src.__support.wctype.wctype_classification_utils
    libc.hdr.types.wint_t)

set(wctype_locale_transform_deps
    ${wctype_transform_deps}
    libc.hdr.types.locale_t)

set(wctype_translate_deps
    libc.src.__support.wctype.wctype_classification_utils
    libc.src.__support.wctype.wctype_descriptors
    libc.hdr.types.wctrans_t
    libc.hdr.types.wint_t)

set(wctype_lookup_deps
    libc.src.__support.wctype.wctype_descriptors
    libc.hdr.types.wctype_t)

set(wctrans_lookup_deps
    libc.src.__support.wctype.wctype_descriptors
    libc.hdr.types.wctrans_t)

add_entrypoint_object(
  iswalnum
  SRCS
    iswalnum.cpp
  HDRS
    iswalnum.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswalnum_l
  SRCS
    iswalnum_l.cpp
  HDRS
    iswalnum_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswalpha
  SRCS
    iswalpha.cpp
  HDRS
    iswalpha.h
  DEPENDS
    libc.src.__support.wctype_utils
)

add_entrypoint_object(
  iswalpha_l
  SRCS
    iswalpha_l.cpp
  HDRS
    iswalpha_l.h
  DEPENDS
    ${wctype_locale_classification_deps}
)

add_entrypoint_object(
  iswblank
  SRCS
    iswblank.cpp
  HDRS
    iswblank.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswblank_l
  SRCS
    iswblank_l.cpp
  HDRS
    iswblank_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswcntrl
  SRCS
    iswcntrl.cpp
  HDRS
    iswcntrl.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswcntrl_l
  SRCS
    iswcntrl_l.cpp
  HDRS
    iswcntrl_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswctype
  SRCS
    iswctype.cpp
  HDRS
    iswctype.h
  DEPENDS
    ${wctype_descriptor_deps}
)

add_entrypoint_object(
  iswctype_l
  SRCS
    iswctype_l.cpp
  HDRS
    iswctype_l.h
  DEPENDS
    ${wctype_locale_descriptor_deps}
)

add_entrypoint_object(
  iswdigit
  SRCS
    iswdigit.cpp
  HDRS
    iswdigit.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswdigit_l
  SRCS
    iswdigit_l.cpp
  HDRS
    iswdigit_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswgraph
  SRCS
    iswgraph.cpp
  HDRS
    iswgraph.h
  DEPENDS
    libc.src.__support.common
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswgraph_l
  SRCS
    iswgraph_l.cpp
  HDRS
    iswgraph_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswlower
  SRCS
    iswlower.cpp
  HDRS
    iswlower.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswlower_l
  SRCS
    iswlower_l.cpp
  HDRS
    iswlower_l.h
  DEPENDS
    ${wctype_locale_classification_deps}
)

add_entrypoint_object(
  iswprint
  SRCS
    iswprint.cpp
  HDRS
    iswprint.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswprint_l
  SRCS
    iswprint_l.cpp
  HDRS
    iswprint_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswpunct
  SRCS
    iswpunct.cpp
  HDRS
    iswpunct.h
  DEPENDS
    libc.src.__support.common
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswpunct_l
  SRCS
    iswpunct_l.cpp
  HDRS
    iswpunct_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswspace
  SRCS
    iswspace.cpp
  HDRS
    iswspace.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswspace_l
  SRCS
    iswspace_l.cpp
  HDRS
    iswspace_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  iswupper
  SRCS
    iswupper.cpp
  HDRS
    iswupper.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswupper_l
  SRCS
    iswupper_l.cpp
  HDRS
    iswupper_l.h
  DEPENDS
    ${wctype_locale_classification_deps}
)

add_entrypoint_object(
  iswxdigit
  SRCS
    iswxdigit.cpp
  HDRS
    iswxdigit.h
  DEPENDS
    ${wctype_basic_deps}
)

add_entrypoint_object(
  iswxdigit_l
  SRCS
    iswxdigit_l.cpp
  HDRS
    iswxdigit_l.h
  DEPENDS
    ${wctype_locale_basic_deps}
)

add_entrypoint_object(
  towctrans
  SRCS
    towctrans.cpp
  HDRS
    towctrans.h
  DEPENDS
    ${wctype_translate_deps}
)

add_entrypoint_object(
  towctrans_l
  SRCS
    towctrans_l.cpp
  HDRS
    towctrans_l.h
  DEPENDS
    ${wctype_translate_deps}
    libc.hdr.types.locale_t
)

add_entrypoint_object(
  towlower
  SRCS
    towlower.cpp
  HDRS
    towlower.h
  DEPENDS
    ${wctype_transform_deps}
)

add_entrypoint_object(
  towlower_l
  SRCS
    towlower_l.cpp
  HDRS
    towlower_l.h
  DEPENDS
    ${wctype_locale_transform_deps}
)

add_entrypoint_object(
  towupper
  SRCS
    towupper.cpp
  HDRS
    towupper.h
  DEPENDS
    ${wctype_transform_deps}
)

add_entrypoint_object(
  towupper_l
  SRCS
    towupper_l.cpp
  HDRS
    towupper_l.h
  DEPENDS
    ${wctype_locale_transform_deps}
)

add_entrypoint_object(
  wctrans
  SRCS
    wctrans.cpp
  HDRS
    wctrans.h
  DEPENDS
    ${wctrans_lookup_deps}
)

add_entrypoint_object(
  wctrans_l
  SRCS
    wctrans_l.cpp
  HDRS
    wctrans_l.h
  DEPENDS
    ${wctrans_lookup_deps}
    libc.hdr.types.locale_t
    libc.src.wctype.wctrans
)

add_entrypoint_object(
  wctype
  SRCS
    wctype.cpp
  HDRS
    wctype.h
  DEPENDS
    ${wctype_lookup_deps}
)

add_entrypoint_object(
  wctype_l
  SRCS
    wctype_l.cpp
  HDRS
    wctype_l.h
  DEPENDS
    ${wctype_lookup_deps}
    libc.hdr.types.locale_t
    libc.src.wctype.wctype
)
