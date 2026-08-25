// libFuzzer entry point for the ITCH parser. The body lives in fuzz_core.hpp so
// the seeded runner in the test suite and this coverage-guided driver exercise
// exactly the same code -- otherwise a bug found here might not be reachable
// from CI, and vice versa.
//
//   cmake -S . -B build-fuzz -DHOTPATH_FUZZ=ON \
//     -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
//   ./build-fuzz/tests/fuzz/fuzz_itch -max_total_time=300 corpus/
//
// Apple's clang does not ship libFuzzer, hence the explicit compiler.
#include "fuzz_core.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  hotpath::fuzz::fuzz_itch_parse(data, size);
  return 0;
}
