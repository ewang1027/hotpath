// libFuzzer entry point for differential fuzzing across the four book designs.
// See fuzz_itch.cpp for how to build and run.
#include "fuzz_core.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  hotpath::fuzz::fuzz_book_differential(data, size);
  return 0;
}
