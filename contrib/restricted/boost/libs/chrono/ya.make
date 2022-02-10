LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt) 

OWNER( 
    antoshkka 
    g:cpp-committee 
    g:cpp-contrib 
) 
 
INCLUDE(${ARCADIA_ROOT}/contrib/restricted/boost/boost_common.inc)

SRCS(
    src/chrono.cpp
    src/process_cpu_clocks.cpp
    src/thread_clock.cpp
)

END()
