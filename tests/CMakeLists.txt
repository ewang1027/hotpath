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
)
target_link_libraries(hotpath_tests PRIVATE hotpath hotpath_instrument Catch2::Catch2WithMain)
target_include_directories(hotpath_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(hotpath_tests)
