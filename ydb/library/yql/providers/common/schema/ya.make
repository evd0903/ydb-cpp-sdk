LIBRARY()

OWNER(g:yql g:yql_ydb_core)

SRCS(
    yql_schema_utils.cpp
)

PEERDIR(
    library/cpp/yson/node
    ydb/library/yql/utils
)

END()
