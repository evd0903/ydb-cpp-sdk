# Generated by devtools/yamaker.

GTEST()

WITHOUT_LICENSE_TEXTS()

OWNER(
    orivej
    g:cpp-contrib
)

LICENSE(MIT)

PEERDIR(
    contrib/libs/fmt
    contrib/libs/fmt/test
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DFMT_HEADER_ONLY=1
    -DGTEST_HAS_STD_WSTRING=1
    -DGTEST_LANG_CXX11=0
    -D_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING=1
)

SRCDIR(contrib/libs/fmt/test)

SRCS(
    header-only-test.cc
    header-only-test2.cc
)

END()
