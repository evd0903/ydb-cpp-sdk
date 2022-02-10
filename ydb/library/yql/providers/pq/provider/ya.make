LIBRARY()

OWNER(
    galaxycrab
    g:yq
    g:yql
)

SRCS(
    yql_pq_datasink.cpp
    yql_pq_datasink_execution.cpp
    yql_pq_datasink_io_discovery.cpp
    yql_pq_datasink_type_ann.cpp
    yql_pq_datasource.cpp
    yql_pq_datasource_type_ann.cpp
    yql_pq_dq_integration.cpp
    yql_pq_io_discovery.cpp
    yql_pq_load_meta.cpp
    yql_pq_logical_opt.cpp
    yql_pq_mkql_compiler.cpp
    yql_pq_physical_optimize.cpp
    yql_pq_provider.cpp
    yql_pq_provider_impl.cpp
    yql_pq_settings.cpp
    yql_pq_topic_key_parser.cpp
    yql_pq_helpers.cpp
)

PEERDIR(
    library/cpp/random_provider
    library/cpp/time_provider
    ydb/core/yq/libs/db_resolver
    ydb/library/yql/ast
    ydb/library/yql/minikql
    ydb/library/yql/minikql/comp_nodes
    ydb/library/yql/minikql/computation
    ydb/library/yql/providers/common/structured_token
    ydb/library/yql/public/udf
    ydb/public/sdk/cpp/client/ydb_driver
    ydb/library/yql/core
    ydb/library/yql/core/type_ann
    ydb/library/yql/dq/expr_nodes
    ydb/library/yql/dq/opt
    ydb/library/yql/providers/common/config
    ydb/library/yql/providers/common/dq
    ydb/library/yql/providers/common/proto
    ydb/library/yql/providers/common/provider
    ydb/library/yql/providers/common/transform
    ydb/library/yql/providers/dq/common
    ydb/library/yql/providers/dq/expr_nodes
    ydb/library/yql/providers/dq/interface
    ydb/library/yql/providers/dq/provider/exec
    ydb/library/yql/providers/pq/cm_client/interface
    ydb/library/yql/providers/pq/common
    ydb/library/yql/providers/pq/expr_nodes
    ydb/library/yql/providers/pq/proto
    ydb/library/yql/providers/result/expr_nodes
)

YQL_LAST_ABI_VERSION()

END()

IF (NOT OPENSOURCE)
    RECURSE_FOR_TESTS(
        ut
    )
ENDIF()
