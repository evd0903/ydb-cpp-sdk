# Generated by devtools/yamaker. 
 
PROGRAM() 
 
WITHOUT_LICENSE_TEXTS()

OWNER(g:cpp-contrib)
 
LICENSE(Apache-2.0) 
 
PEERDIR( 
    contrib/libs/grpc/grpc 
    contrib/libs/grpc/src/core/lib 
    contrib/libs/grpc/third_party/address_sorting 
    contrib/libs/grpc/third_party/upb
) 
 
ADDINCL( 
    ${ARCADIA_BUILD_ROOT}/contrib/libs/grpc
    contrib/libs/grpc 
    contrib/libs/grpc/include 
) 
 
NO_COMPILER_WARNINGS() 
 
SRCDIR(contrib/libs/grpc/tools/codegen/core) 
 
IF (OS_LINUX OR OS_DARWIN) 
    CFLAGS(
        -DGRPC_POSIX_FORK_ALLOW_PTHREAD_ATFORK=1
    )
ENDIF() 
 
SRCS( 
    gen_hpack_tables.cc 
) 
 
END() 
