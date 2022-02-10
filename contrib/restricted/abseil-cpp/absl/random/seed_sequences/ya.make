# Generated by devtools/yamaker.

LIBRARY()

WITHOUT_LICENSE_TEXTS() 
 
OWNER(g:cpp-contrib)

LICENSE(Apache-2.0)

PEERDIR(
    contrib/restricted/abseil-cpp/absl/base
    contrib/restricted/abseil-cpp/absl/base/internal/raw_logging
    contrib/restricted/abseil-cpp/absl/base/internal/spinlock_wait
    contrib/restricted/abseil-cpp/absl/base/internal/throw_delegate
    contrib/restricted/abseil-cpp/absl/base/log_severity
    contrib/restricted/abseil-cpp/absl/numeric
    contrib/restricted/abseil-cpp/absl/random/internal/pool_urbg
    contrib/restricted/abseil-cpp/absl/random/internal/randen
    contrib/restricted/abseil-cpp/absl/random/internal/randen_detect
    contrib/restricted/abseil-cpp/absl/random/internal/randen_hwaes
    contrib/restricted/abseil-cpp/absl/random/internal/randen_round_keys
    contrib/restricted/abseil-cpp/absl/random/internal/randen_slow
    contrib/restricted/abseil-cpp/absl/random/internal/seed_material
    contrib/restricted/abseil-cpp/absl/random/seed_gen_exception
    contrib/restricted/abseil-cpp/absl/strings
    contrib/restricted/abseil-cpp/absl/strings/internal/absl_strings_internal
    contrib/restricted/abseil-cpp/absl/types/bad_optional_access
)

ADDINCL(
    GLOBAL contrib/restricted/abseil-cpp
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DNOMINMAX
)

SRCDIR(contrib/restricted/abseil-cpp/absl/random)

SRCS(
    seed_sequences.cc
)

END()
