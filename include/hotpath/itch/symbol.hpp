#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace hotpath::itch {

// ITCH stock symbols are exactly 8 bytes, right-padded with spaces. Holding
// them as a raw 8-byte quantity lets equality and hashing be a single 64-bit
// operation instead of a string compare.
struct Symbol {
  std::uint64_t raw{0};

  static Symbol from_bytes(const std::uint8_t* p) noexcept {
    Symbol s;
    std::memcpy(&s.raw, p, 8);   // byte order irrelevant: opaque identity
    return s;
  }

  static Symbol from_text(std::string_view t) noexcept {
    char buf[8];
    std::memset(buf, ' ', sizeof buf);
    std::memcpy(buf, t.data(), t.size() < 8 ? t.size() : 8);
    Symbol s;
    std::memcpy(&s.raw, buf, 8);
    return s;
  }

  [[nodiscard]] std::string text() const {
    char buf[8];
    std::memcpy(buf, &raw, 8);
    std::size_t n = 8;
    while (n > 0 && buf[n - 1] == ' ') --n;
    return std::string(buf, n);
  }

  friend bool operator==(Symbol a, Symbol b) noexcept { return a.raw == b.raw; }
  friend bool operator!=(Symbol a, Symbol b) noexcept { return a.raw != b.raw; }
  friend bool operator<(Symbol a, Symbol b) noexcept { return a.raw < b.raw; }
};

struct SymbolHash {
  std::size_t operator()(Symbol s) const noexcept {
    // splitmix64 finaliser -- symbols are dense ASCII and hash poorly raw.
    std::uint64_t x = s.raw;
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<std::size_t>(x);
  }
};

} // namespace hotpath::itch
