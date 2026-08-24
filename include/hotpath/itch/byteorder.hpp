#pragma once
#include <cstdint>
#include <cstring>

namespace hotpath::itch {

// ITCH is big-endian. We read through memcpy rather than casting the mapped
// bytes to a packed struct: the file offsets are not aligned, and a
// reinterpret_cast to a misaligned uint32_t* is UB even on architectures that
// tolerate it. Clang lowers each of these to a single load + rev instruction,
// so the safe version costs nothing.

[[nodiscard]] inline std::uint16_t rd_u16(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap16(v);
}

[[nodiscard]] inline std::uint32_t rd_u32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap32(v);
}

[[nodiscard]] inline std::uint64_t rd_u64(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap64(v);
}

// ITCH timestamps are 6 bytes (nanoseconds since midnight). Read 8 and shift
// rather than assembling byte-by-byte.
[[nodiscard]] inline std::uint64_t rd_u48(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  std::memcpy(reinterpret_cast<std::uint8_t*>(&v) + 2, p, 6);
  return __builtin_bswap64(v);
}

} // namespace hotpath::itch
