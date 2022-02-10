# Generated by devtools/yamaker.

LIBRARY() 
 
WITHOUT_LICENSE_TEXTS()

OWNER(
    orivej
    g:cpp-contrib
)

LICENSE(BSD-3-Clause)

PEERDIR( 
    contrib/libs/pcre
) 
 
ADDINCL(contrib/libs/pcre)
 
NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(-DHAVE_CONFIG_H)

SRCDIR(contrib/libs/pcre)

SRCS( 
    pcre_scanner.cc 
    pcre_stringpiece.cc 
    pcrecpp.cc
) 
 
END() 
