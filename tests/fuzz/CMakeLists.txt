# libFuzzer targets, off by default: Apple's clang does not ship libFuzzer, so
# these need an LLVM toolchain (brew install llvm). The seeded runner in
# test_fuzz.cpp covers the same code with no toolchain requirement and runs on
# every build, which is what keeps CI honest; these are for deeper campaigns.
add_executable(fuzz_itch fuzz_itch.cpp)
add_executable(fuzz_book fuzz_book.cpp)
foreach(t fuzz_itch fuzz_book)
  target_link_libraries(${t} PRIVATE hotpath)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_compile_options(${t} PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
  target_link_options(${t} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
