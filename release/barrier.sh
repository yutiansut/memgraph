#!/bin/bash

binary_name=$1

cd ..

release_folder="barrier_$binary_name"
release_path="release/$release_folder"
compile_template_path="template/barrier_template_code_cpu.cpp"

# MKDIR
mkdir -p $release_path/compiled/cpu/hardcode

mkdir -p $release_path/include/barrier
mkdir -p $release_path/include/cypher
mkdir -p $release_path/include/io/network
mkdir -p $release_path/include/logging
mkdir -p $release_path/include/mvcc
mkdir -p $release_path/include/query_engine
mkdir -p $release_path/include/storage/model/properties/traversers
mkdir -p $release_path/include/storage/model/properties/utils
mkdir -p $release_path/include/utils/datetime
mkdir -p $release_path/include/utils/exceptions
mkdir -p $release_path/include/utils/iterator
mkdir -p $release_path/include/utils/memory

mkdir -p $release_path/template

# COPY
# dressipi query
# TODO: for loop
hardcoded_queries="135757557963690525.cpp"
for query in $hardcoded_queries
do
    cp build/compiled/cpu/hardcode/$query $release_path/compiled/cpu/hardcode/$query
done

cp src/query_engine/$compile_template_path $release_path/$compile_template_path

paths="barrier/barrier.hpp barrier/common.hpp storage/model/properties/floating.hpp storage/model/properties/all.hpp storage/model/properties/bool.hpp storage/model/properties/traversers/consolewriter.hpp storage/model/properties/traversers/jsonwriter.hpp storage/model/properties/array.hpp storage/model/properties/property_family.hpp storage/model/properties/property.hpp storage/model/properties/properties.hpp storage/model/properties/integral.hpp storage/model/properties/double.hpp storage/model/properties/string.hpp storage/model/properties/utils/math_operations.hpp storage/model/properties/utils/unary_negation.hpp storage/model/properties/utils/modulo.hpp storage/model/properties/float.hpp storage/model/properties/null.hpp storage/model/properties/flags.hpp storage/model/properties/int32.hpp storage/model/properties/number.hpp storage/model/properties/int64.hpp logging/default.hpp logging/log.hpp logging/logger.hpp logging/levels.hpp io/network/addrinfo.hpp io/network/network_error.hpp io/network/socket.hpp mvcc/id.hpp utils/exceptions/basic_exception.hpp utils/border.hpp utils/total_ordering.hpp utils/auto_scope.hpp utils/crtp.hpp utils/order.hpp utils/likely.hpp utils/option.hpp utils/option_ptr.hpp utils/memory/block_allocator.hpp utils/memory/stack_allocator.hpp utils/iterator/query.hpp utils/iterator/composable.hpp utils/iterator/for_all.hpp utils/iterator/range_iterator.hpp utils/iterator/limited_map.hpp utils/iterator/iterator_accessor.hpp utils/iterator/count.hpp utils/iterator/iterator_base.hpp utils/iterator/filter.hpp utils/iterator/inspect.hpp utils/iterator/accessor.hpp utils/iterator/map.hpp utils/iterator/virtual_iter.hpp utils/iterator/flat_map.hpp utils/iterator/lambda_iterator.hpp utils/iterator/iterator.hpp utils/stacktrace.hpp utils/datetime/datetime_error.hpp utils/datetime/timestamp.hpp utils/reference_wrapper.hpp utils/underlying_cast.hpp query_engine/i_code_cpu.hpp query_engine/query_stripped.hpp"

for path in $paths
do
    cp include/$path $release_path/include/$path
done

cp config/memgraph.yaml $release_path/config.yaml

cp build/$binary_name $release_path/$binary_name
cp build/libmemgraph_pic.a $release_path/libmemgraph_pic.a
cp build/libbarrier_pic.a $release_path/libbarrier_pic.a

echo "DONE"
