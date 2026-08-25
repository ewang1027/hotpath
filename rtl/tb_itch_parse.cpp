// Co-simulation: the SystemVerilog ITCH parser against the C++ one.
//
// Both are fed the same byte stream and every decoded field is compared. This
// is the same argument the four book designs make -- two independent
// implementations agreeing over a large input is evidence neither is wrong in
// an interesting way -- except that here one of them is hardware.
//
// It also checks the malformed-input strobes, because that is where the C++
// side had two out-of-bounds reads (docs/BUILDLOG.md): a parser that silently
// accepts an under-length message hands downstream logic fields it never
// received. In RTL the same mistake reads stale flops instead of stale memory,
// which is harder to notice and impossible for a sanitizer to catch.
#include "Vitch_parse.h"
#include "verilated.h"

#include "fixture.hpp"
#include "fuzz_core.hpp"
#include "hotpath/itch/mapped_file.hpp"
#include "hotpath/itch/reader.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hotpath;
using namespace hotpath::itch;

namespace {

struct Decoded {
  std::uint8_t  type;
  std::uint16_t length;
  std::uint16_t locate;
  std::uint64_t timestamp;
  std::uint64_t order_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
  bool          side_buy;
  bool          book_event;
};

struct RtlRun {
  std::vector<Decoded> msgs;
  std::uint64_t shorts{0};
  std::uint64_t zero_lens{0};
  std::uint64_t cycles{0};
};

void tick(Vitch_parse& dut) {
  dut.clk = 0; dut.eval();
  dut.clk = 1; dut.eval();
}

RtlRun run_rtl(Vitch_parse& dut, const std::vector<std::uint8_t>& bytes) {
  RtlRun r;
  dut.rst = 1; dut.s_valid = 0; dut.s_byte = 0;
  tick(dut); tick(dut);
  dut.rst = 0;

  for (std::uint8_t b : bytes) {
    dut.s_valid = 1;
    dut.s_byte  = b;
    tick(dut);
    ++r.cycles;
    if (dut.m_valid) {
      r.msgs.push_back(Decoded{
          static_cast<std::uint8_t>(dut.m_type), static_cast<std::uint16_t>(dut.m_length),
          static_cast<std::uint16_t>(dut.m_locate), dut.m_timestamp, dut.m_order_ref,
          dut.m_new_ref, dut.m_shares, dut.m_price,
          dut.m_side_buy != 0, dut.m_book_event != 0});
    }
    if (dut.e_short) ++r.shorts;
    if (dut.e_zero_len) ++r.zero_lens;
  }
  // Drain: the decode strobe for the final message lands the cycle after its
  // last byte, so stopping at the last byte would silently lose it.
  dut.s_valid = 0;
  for (int i = 0; i < 4; ++i) {
    tick(dut);
    if (dut.m_valid) {
      r.msgs.push_back(Decoded{
          static_cast<std::uint8_t>(dut.m_type), static_cast<std::uint16_t>(dut.m_length),
          static_cast<std::uint16_t>(dut.m_locate), dut.m_timestamp, dut.m_order_ref,
          dut.m_new_ref, dut.m_shares, dut.m_price,
          dut.m_side_buy != 0, dut.m_book_event != 0});
    }
    if (dut.e_short) ++r.shorts;
    if (dut.e_zero_len) ++r.zero_lens;
  }
  return r;
}

// The same stream through the C++ parser, decoded into the same shape.
// `stats_out` (optional) receives the reference's view of the stream.
std::vector<Decoded> run_cpp(const std::vector<std::uint8_t>& bytes,
                             ReaderStats* stats_out = nullptr) {
  std::vector<Decoded> out;
  Reader rd(bytes.data(), bytes.size());
  RawMessage m{};
  while (rd.next(m)) {
    const Header h{m.body};
    Decoded d{};
    d.type = static_cast<std::uint8_t>(m.type());
    d.length = m.length;
    d.locate = h.locate();
    d.timestamp = h.timestamp();
    switch (m.type()) {
      case 'A': case 'F': {
        const AddOrderView v{m.body};
        d.order_ref = v.order_ref(); d.shares = v.shares(); d.price = v.price();
        d.side_buy = v.side() == Side::Buy; d.book_event = true;
        break;
      }
      case 'E': case 'C': {
        const OrderExecutedView v{m.body};
        d.order_ref = v.order_ref(); d.shares = v.executed_shares();
        d.book_event = true;
        break;
      }
      case 'X': {
        const OrderCancelView v{m.body};
        d.order_ref = v.order_ref(); d.shares = v.cancelled_shares();
        d.book_event = true;
        break;
      }
      case 'D': {
        d.order_ref = OrderDeleteView{m.body}.order_ref();
        d.book_event = true;
        break;
      }
      case 'U': {
        const OrderReplaceView v{m.body};
        d.order_ref = v.original_ref(); d.new_ref = v.new_ref();
        d.shares = v.shares(); d.price = v.price();
        d.book_event = true;
        break;
      }
      default:
        // Reference-data types: the RTL decodes the common header and reports
        // book_event=0, which is all a feed handler needs at line rate.
        break;
    }
    out.push_back(d);
  }
  if (stats_out) *stats_out = rd.stats();
  return out;
}

int compare(const std::vector<Decoded>& rtl, const std::vector<Decoded>& cpp_,
            const char* label) {
  if (rtl.size() != cpp_.size()) {
    std::printf("  %-22s MISMATCH: rtl decoded %zu, c++ decoded %zu\n",
                label, rtl.size(), cpp_.size());
    return 1;
  }
  for (std::size_t i = 0; i < rtl.size(); ++i) {
    const Decoded& a = rtl[i];
    const Decoded& b = cpp_[i];
#define CHECK(field, fmt)                                                        \
    if (a.field != b.field) {                                                    \
      std::printf("  %-22s MISMATCH at message %zu (type '%c'): " #field         \
                  " rtl=" fmt " c++=" fmt "\n", label, i, b.type,                \
                  (std::uint64_t)a.field, (std::uint64_t)b.field);               \
      return 1;                                                                  \
    }
    CHECK(type, "%" PRIu64) CHECK(length, "%" PRIu64) CHECK(locate, "%" PRIu64)
    CHECK(timestamp, "%" PRIu64) CHECK(order_ref, "%" PRIu64)
    CHECK(new_ref, "%" PRIu64) CHECK(shares, "%" PRIu64) CHECK(price, "%" PRIu64)
    CHECK(side_buy, "%" PRIu64) CHECK(book_event, "%" PRIu64)
#undef CHECK
  }
  std::printf("  %-22s %zu messages, every field identical\n", label, rtl.size());
  return 0;
}


} // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  Vitch_parse dut;
  int fail = 0;
  std::string real_file;
  std::size_t real_bytes = 4u << 20;
  int fuzz_iters = 2000;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--file" && i + 1 < argc) real_file = argv[++i];
    else if (a == "--bytes" && i + 1 < argc) real_bytes = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--fuzz" && i + 1 < argc) fuzz_iters = std::atoi(argv[++i]);
  }

  std::printf("ITCH parser co-simulation: SystemVerilog vs C++\n\n");

  // ---- 1. one of every book message type, hand-built with distinct fields ----
  {
    test::ItchBuilder b;
    b.add_order(0x1122334455667788ull, Side::Buy, 12345, "AAPL", 1502500, 7, 0xAABBCCDDull);
    b.add_order(0x2233445566778899ull, Side::Sell, 500, "MSFT", 3000000, 9, 0xBBCCDDEEull);
    b.order_executed(0x1122334455667788ull, 50, 0xDEADBEEFull, 7, 0x1111ull);
    b.order_cancel(0x2233445566778899ull, 25, 9, 0x2222ull);
    b.order_delete(0x1122334455667788ull, 7, 0x3333ull);
    b.order_replace(0x2233445566778899ull, 0x99AABBCCDDEEFF00ull, 77, 999999, 9, 0x4444ull);
    b.system_event('O', 0x5555ull);
    const auto& bytes = b.bytes();
    const auto r = run_rtl(dut, bytes);
    fail |= compare(r.msgs, run_cpp(bytes), "one of each type");
    std::printf("  %-22s %" PRIu64 " cycles for %zu bytes (1 byte/cycle)\n",
                "throughput", r.cycles, bytes.size());
  }

  // ---- 2. a long randomised stream ----
  {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    auto next = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    test::ItchBuilder b;
    for (int i = 0; i < 4000; ++i) {
      const OrderId id = 1 + (next() % 100000);
      switch (next() % 7) {
        case 0: b.add_order(id, (next() & 1) ? Side::Buy : Side::Sell,
                            static_cast<Qty>(1 + next() % 65535), "AAPL",
                            static_cast<Price>(next() % 4000000), 
                            static_cast<StockLocate>(next() % 9000),
                            next() % 100000000000ull); break;
        case 1: b.order_executed(id, static_cast<Qty>(1 + next() % 5000), next(),
                                 static_cast<StockLocate>(next() % 9000), next() % 1000000); break;
        case 2: b.order_cancel(id, static_cast<Qty>(1 + next() % 5000),
                               static_cast<StockLocate>(next() % 9000), next() % 1000000); break;
        case 3: b.order_delete(id, static_cast<StockLocate>(next() % 9000), next() % 1000000); break;
        case 4: b.order_replace(id, id + 7, static_cast<Qty>(1 + next() % 5000),
                                static_cast<Price>(next() % 4000000),
                                static_cast<StockLocate>(next() % 9000), next() % 1000000); break;
        default: b.system_event(static_cast<char>('A' + next() % 26), next() % 1000000); break;
      }
    }
    const auto& bytes = b.bytes();
    const auto r = run_rtl(dut, bytes);
    fail |= compare(r.msgs, run_cpp(bytes), "4000 random messages");
  }

  // ---- 3. malformed input: both must reject, not decode ----
  {
    test::ItchBuilder b;
    b.add_order(1, Side::Buy, 100, "AAPL", 1000000);
    b.add({std::uint8_t('~'), 0, 0, 0, 0});                 // 5 bytes: under the header
    b.add({std::uint8_t('A'), 0, 0, 0});                    // 4 bytes claiming to be an Add
    b.order_delete(1);
    const auto& bytes = b.bytes();
    const auto r = run_rtl(dut, bytes);
    const auto c = run_cpp(bytes);
    std::printf("  %-22s rtl e_short=%" PRIu64 ", rtl decoded=%zu, c++ decoded=%zu\n",
                "malformed input", r.shorts, r.msgs.size(), c.size());
    if (r.shorts != 2 || r.msgs.size() != c.size()) {
      std::printf("  %-22s MISMATCH: both must reject the two short messages\n", "malformed input");
      fail = 1;
    } else {
      fail |= compare(r.msgs, c, "malformed input");
    }
  }

  // ---- 3b. differential fuzzing: RTL against the C++ golden model ----
  //
  // The same generators the software fuzz targets use (tests/fuzz_core.hpp),
  // pointed at the hardware. This is what a verification team does with a
  // reference model, and it is stronger than either side alone: a shared
  // misreading of the spec cannot hide, because the two implementations were
  // written in different languages against different mental models.
  if (fuzz_iters > 0) {
    std::printf("\n  fuzzing RTL vs C++ over %d generated inputs\n", fuzz_iters);
    std::uint64_t total_bytes = 0, total_msgs = 0, total_short = 0, mismatches = 0;
    for (int i = 0; i < fuzz_iters && mismatches < 5; ++i) {
      hotpath::fuzz::Rng r(0xF0F0F0F0ull + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull);
      std::vector<std::uint8_t> input =
          (r.next() & 1) ? hotpath::fuzz::random_bytes(r, r.below(400) + 1)
                         : hotpath::fuzz::mutated_itch(r, 1 + r.below(20));
      if (input.empty()) continue;

      // Compare over the prefix the REFERENCE actually parsed. The two stop for
      // different reasons on a malformed stream -- C++ halts at a zero-length
      // frame, the RTL resynchronises past it -- and that is a difference in
      // recovery policy, not in decoding. Feeding both exactly what the
      // reference consumed isolates the decode.
      ReaderStats st{};
      const auto cpp_msgs = run_cpp(input, &st);
      if (st.bytes == 0 || st.bytes > input.size()) continue;
      input.resize(static_cast<std::size_t>(st.bytes));

      const auto r_rtl = run_rtl(dut, input);
      total_bytes += input.size();
      total_msgs += r_rtl.msgs.size();
      total_short += r_rtl.shorts;

      bool bad = false;
      if (r_rtl.msgs.size() != cpp_msgs.size()) bad = true;
      else for (std::size_t k = 0; k < r_rtl.msgs.size() && !bad; ++k) {
        const Decoded& a = r_rtl.msgs[k];
        const Decoded& b = cpp_msgs[k];
        bad = a.type != b.type || a.length != b.length || a.locate != b.locate ||
              a.timestamp != b.timestamp || a.order_ref != b.order_ref ||
              a.new_ref != b.new_ref || a.shares != b.shares ||
              a.price != b.price || a.side_buy != b.side_buy ||
              a.book_event != b.book_event;
      }
      // Both must also agree on what they REJECTED, not merely on what they
      // accepted: a parser that quietly emits an under-length message is the
      // exact bug that produced two out-of-bounds reads on the C++ side.
      if (!bad && r_rtl.shorts != st.short_message) bad = true;

      if (bad) {
        ++mismatches;
        std::printf("    MISMATCH on input %d (%zu bytes): rtl %zu msgs / %" PRIu64
                    " short, c++ %zu msgs / %" PRIu64 " short\n",
                    i, input.size(), r_rtl.msgs.size(), r_rtl.shorts,
                    cpp_msgs.size(), st.short_message);
      }
    }
    std::printf("  %-22s %d inputs, %" PRIu64 " bytes, %" PRIu64 " messages, %"
                PRIu64 " rejected, %" PRIu64 " mismatches\n",
                "RTL fuzz", fuzz_iters, total_bytes, total_msgs, total_short, mismatches);
    if (mismatches) fail = 1;
  }

  // ---- 4. real NASDAQ bytes ----
  //
  // Synthetic streams exercise the offsets; only the real feed exercises the
  // message mix, the reference-data types interleaved with book updates, and
  // the actual distribution of lengths. Truncated to a prefix because Verilator
  // simulates a byte per cycle and the full day is 8.25 GB.
  if (!real_file.empty()) {
    itch::MappedFile f(real_file);
    const std::size_t n = real_bytes < f.size() ? real_bytes : f.size();
    // Trim to a whole message so the tail is not a spurious mismatch.
    std::size_t usable = 0;
    {
      Reader probe(f.data(), n);
      RawMessage m{};
      while (probe.next(m)) usable = static_cast<std::size_t>(m.body + m.length - f.data());
    }
    std::vector<std::uint8_t> bytes(f.data(), f.data() + usable);
    std::printf("\n  real data: %s, first %.2f MB (%zu whole messages worth)\n",
                real_file.c_str(), static_cast<double>(usable) / 1e6, usable);
    const auto r = run_rtl(dut, bytes);
    fail |= compare(r.msgs, run_cpp(bytes), "real NASDAQ ITCH");
  }

  std::printf("\nCO-SIMULATION: %s\n", fail ? "FAIL" : "PASS");
  return fail;
}
