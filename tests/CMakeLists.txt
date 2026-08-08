include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(qm_tests
  test_parser.cpp
  test_simd.cpp
  test_spsc.cpp
  test_book.cpp
  test_shm.cpp
  test_config.cpp
)
target_link_libraries(qm_tests PRIVATE quotemesh GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(qm_tests)
