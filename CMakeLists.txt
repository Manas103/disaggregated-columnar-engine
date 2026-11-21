cmake_minimum_required(VERSION 3.16)
project(disaggregated_columnar_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_ASAN "Build with Address/UndefinedBehavior sanitizers" OFF)

add_library(dce INTERFACE)
target_include_directories(dce INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(dce INTERFACE /O2 /W4 /EHsc)
else()
  target_compile_options(dce INTERFACE -O2 -Wall -Wextra -Wpedantic)
endif()

if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_executable(dce_test tests/test_main.cpp)
target_link_libraries(dce_test PRIVATE dce Threads::Threads)

add_executable(bench_pruning bench/bench_pruning.cpp)
target_link_libraries(bench_pruning PRIVATE dce Threads::Threads)

add_executable(bench_cache_latency bench/bench_cache_latency.cpp)
target_link_libraries(bench_cache_latency PRIVATE dce Threads::Threads)

add_executable(bench_cache_residency bench/bench_cache_residency.cpp)
target_link_libraries(bench_cache_residency PRIVATE dce Threads::Threads)

enable_testing()
add_test(NAME dce_checks COMMAND dce_test)
