cmake_minimum_required(VERSION 3.16)
project(kv_store LANGUAGES CXX)
add_executable(kvstore main.cpp)
add_executable(kvserver server.cpp)
add_executable(kvserver_multi server_multi.cpp)
add_executable(kvbench bench/kvbench.cpp)
find_package(Threads REQUIRED)
target_link_libraries(kvbench PRIVATE Threads::Threads)
foreach(target kvstore kvserver kvserver_multi kvbench)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME tcp_protocol COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tcp.py $<TARGET_FILE:kvserver>)
    add_test(NAME tcp_multi COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_multi.py $<TARGET_FILE:kvserver_multi>)
    add_test(NAME persistence COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_persistence.py $<TARGET_FILE:kvserver_multi>)
    add_test(NAME compaction COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compaction.py $<TARGET_FILE:kvserver_multi>)
    # Only checks that the benchmark runs; its numbers are meaningless.
    add_test(NAME bench_smoke COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/bench/run_bench.py --quick
        --server $<TARGET_FILE:kvserver_multi> --client $<TARGET_FILE:kvbench>
        --out ${CMAKE_CURRENT_BINARY_DIR}/bench-smoke.json)
    set_tests_properties(tcp_protocol tcp_multi persistence compaction bench_smoke
        PROPERTIES TIMEOUT 30)
endif()
