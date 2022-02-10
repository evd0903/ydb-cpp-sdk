# Generated by devtools/yamaker.

LIBRARY()

OWNER(g:cpp-contrib)

LICENSE(Apache-2.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/restricted/abseil-cpp/absl/base 
    contrib/restricted/abseil-cpp/absl/base/internal/raw_logging
    contrib/restricted/abseil-cpp/absl/base/internal/spinlock_wait 
    contrib/restricted/abseil-cpp/absl/base/internal/throw_delegate
    contrib/restricted/abseil-cpp/absl/base/log_severity
    contrib/restricted/abseil-cpp/absl/numeric
    contrib/restricted/abseil-cpp/absl/strings/internal/absl_strings_internal
)

ADDINCL(
    GLOBAL contrib/restricted/abseil-cpp
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DNOMINMAX
)

SRCS(
    ascii.cc
    charconv.cc
    escaping.cc
    internal/charconv_bigint.cc
    internal/charconv_parse.cc
    internal/memutil.cc
    match.cc
    numbers.cc
    str_cat.cc
    str_replace.cc
    str_split.cc
    string_view.cc
    substitute.cc
)

END()
