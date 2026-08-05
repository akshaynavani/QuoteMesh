cmake_minimum_required(VERSION 3.20)
project(QuoteMesh VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QM_NATIVE        "Compile with -march=native (enables AVX2 where available)" ON)
option(QM_FORCE_SCALAR  "Disable all SIMD code paths (reference build)" OFF)
option(QM_BUILD_TESTS   "Build unit tests" ON)
option(QM_BUILD_BENCH   "Build benchmarks" ON)
option(QM_BUILD_APPS    "Build applications" ON)

find_package(Threads REQUIRED)

add_library(qm_warnings INTERFACE)
target_compile_options(qm_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

add_library(quotemesh STATIC
  src/symbol_table.cpp
  src/wire_format.cpp
  src/scalar_parser.cpp
  src/simd_parser.cpp
  src/synthetic_feed.cpp
  src/udp_feed.cpp
  src/feed_handler.cpp
  src/consolidated_book.cpp
  src/aggregator.cpp
  src/shm_region.cpp
)
target_include_directories(quotemesh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quotemesh PUBLIC Threads::Threads PRIVATE qm_warnings)

if(QM_NATIVE AND NOT QM_FORCE_SCALAR)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" QM_HAS_MARCH_NATIVE)
  if(QM_HAS_MARCH_NATIVE)
    target_compile_options(quotemesh PUBLIC -march=native)
  endif()
endif()
if(QM_FORCE_SCALAR)
  target_compile_definitions(quotemesh PUBLIC QM_FORCE_SCALAR=1)
endif()

if(QM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(QM_BUILD_BENCH)
  add_executable(bench_parser bench/bench_parser.cpp)
  target_link_libraries(bench_parser PRIVATE quotemesh)
endif()
