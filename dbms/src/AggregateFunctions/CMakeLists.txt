include(${ClickHouse_SOURCE_DIR}/cmake/dbms_glob_sources.cmake)
add_headers_and_sources(clickhouse_aggregate_functions .)

list(REMOVE_ITEM clickhouse_aggregate_functions_sources
    AggregateFunctionFactory.cpp
    AggregateFunctionState.cpp
    AggregateFunctionArray.cpp
    AggregateFunctionNull.cpp
    AggregateFunctionForEach.cpp
    AggregateFunctionIf.cpp
    AggregateFunctionMerge.cpp
    AggregateFunctionCount.cpp
)

list(REMOVE_ITEM clickhouse_aggregate_functions_headers
    AggregateFunction.h
    AggregateFunctionFactory.h
    AggregateFunctionState.h
    AggregateFunctionArray.h
    AggregateFunctionNull.h
    AggregateFunctionForEach.h
    AggregateFunctionIf.h
    AggregateFunctionMerge.h
    AggregateFunctionCount.h
)

add_library(clickhouse_aggregate_functions ${clickhouse_aggregate_functions_sources})
