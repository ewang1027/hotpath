include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(hotpath_tests
  test_open_hash.cpp
  test_itch.cpp
  test_book.cpp
  test_ring.cpp
  test_sim.cpp
  test_market_maker.cpp
  test_signals.cpp
  test_bootstrap.cpp
  test_fuzz.cpp
)
target_link_libraries(hotpath_tests PRIVATE hotpath hotpath_instrument Catch2::Catch2WithMain)
target_include_directories(hotpath_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Long-running campaign driver. Not a ctest target -- it is meant to be pointed
# at a machine for minutes or hours, which is not what a test suite is for.
add_executable(fuzz_campaign fuzz_campaign.cpp)
target_link_libraries(fuzz_campaign PRIVATE hotpath)
target_include_directories(fuzz_campaign PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(hotpath_tests)

option(HOTPATH_FUZZ "Build libFuzzer targets (needs an LLVM toolchain, not Apple clang)" OFF)
if(HOTPATH_FUZZ)
  add_subdirectory(fuzz)
endif()
