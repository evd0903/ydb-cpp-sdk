# Generated by devtools/yamaker.

LIBRARY()

WITHOUT_LICENSE_TEXTS() 
 
OWNER(g:cpp-contrib)

LICENSE(Apache-2.0)

PEERDIR(
    contrib/restricted/abseil-cpp/absl/base/internal/raw_logging
    contrib/restricted/abseil-cpp/absl/base/log_severity
)

ADDINCL(
    GLOBAL contrib/restricted/abseil-cpp
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DNOMINMAX
)

SRCDIR(contrib/restricted/abseil-cpp/absl/types)

SRCS(
    bad_any_cast.cc
)

END()
