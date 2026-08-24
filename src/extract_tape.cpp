// Extract per-symbol book-event tapes from a raw ITCH 5.0 file.
//
// The routing problem this solves: ITCH's Execute/Cancel/Delete/Replace
// messages carry an order reference and nothing else -- no symbol field. The
// only way to know which book a delete belongs to is to have seen the
// corresponding Add. So a single global order_ref -> stock_locate index has to
// be maintained across the whole file even when we only care about one symbol.
#include "hotpath/bench/counters.hpp"
#include "hotpath/book/events.hpp"
#include "hotpath/core/open_hash.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/itch/mapped_file.hpp"
#include "hotpath/itch/reader.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hotpath;
using namespace hotpath::itch;
using namespace hotpath::book;

namespace {

struct Target {
  Symbol symbol;
  StockLocate locate{0};
  std::vector<BookEvent> events;
};

} // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string outdir = ".";
  std::vector<std::string> want;
  std::size_t table_pow2 = 1u << 24;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) outdir = argv[++i];
    else if (a == "--symbol" && i + 1 < argc) want.emplace_back(argv[++i]);
    else if (a == "--table" && i + 1 < argc) table_pow2 = std::strtoull(argv[++i], nullptr, 10);
    else path = a;
  }
  if (path.empty() || want.empty()) {
    std::fprintf(stderr,
        "usage: extract_tape <file.NASDAQ_ITCH50> --symbol SYM [--symbol SYM ...] "
        "[--out DIR] [--table SLOTS]\n");
    return 2;
  }

  request_performance_cores();
  MappedFile file(path);

  // locate -> target index, filled in from Stock Directory ('R') messages.
  std::unordered_map<StockLocate, std::size_t> locate_to_target;
  std::unordered_map<std::uint64_t, std::size_t> symbol_to_target;
  std::vector<Target> targets(want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    targets[i].symbol = Symbol::from_text(want[i]);
    symbol_to_target[targets[i].symbol.raw] = i;
  }

  // Global routing index. Value is the target slot + 1, or 0 for "an order in
  // a symbol we do not care about" -- we still must track those, because a
  // reference we have never seen would otherwise look like corruption.
  OpenHashMap<std::uint16_t> route(table_pow2);

  Reader reader(file.data(), file.size());
  RawMessage m{};
  std::uint64_t routed = 0, unroutable = 0;

  while (reader.next(m)) {
    const char t = m.type();
    switch (t) {
      case 'R': {
        const StockDirectoryView v{m.body};
        auto it = symbol_to_target.find(v.stock().raw);
        if (it != symbol_to_target.end()) {
          targets[it->second].locate = v.locate();
          locate_to_target[v.locate()] = it->second;
        }
        break;
      }
      case 'A':
      case 'F': {
        const AddOrderView v{m.body};
        auto it = locate_to_target.find(v.locate());
        const std::uint16_t slot =
            it == locate_to_target.end() ? 0 : static_cast<std::uint16_t>(it->second + 1);
        route.insert(v.order_ref(), slot);
        if (slot) {
          targets[slot - 1].events.push_back(BookEvent{
              v.timestamp(), v.order_ref(), 0, v.price(), v.shares(),
              EventType::Add, v.side(), 0});
          ++routed;
        }
        break;
      }
      case 'E':
      case 'C': {
        const OrderExecutedView v{m.body};
        const std::uint16_t* slot = route.find(v.order_ref());
        if (!slot) { ++unroutable; break; }
        if (*slot) {
          targets[*slot - 1].events.push_back(BookEvent{
              v.timestamp(), v.order_ref(), 0, 0, v.executed_shares(),
              EventType::Execute, Side::Buy, 0});
          ++routed;
        }
        break;
      }
      case 'X': {
        const OrderCancelView v{m.body};
        const std::uint16_t* slot = route.find(v.order_ref());
        if (!slot) { ++unroutable; break; }
        if (*slot) {
          targets[*slot - 1].events.push_back(BookEvent{
              v.timestamp(), v.order_ref(), 0, 0, v.cancelled_shares(),
              EventType::Cancel, Side::Buy, 0});
          ++routed;
        }
        break;
      }
      case 'D': {
        const OrderDeleteView v{m.body};
        const std::uint16_t* slot = route.find(v.order_ref());
        if (!slot) { ++unroutable; break; }
        if (*slot) {
          targets[*slot - 1].events.push_back(BookEvent{
              v.timestamp(), v.order_ref(), 0, 0, 0,
              EventType::Delete, Side::Buy, 0});
          ++routed;
        }
        route.erase(v.order_ref());
        break;
      }
      case 'U': {
        const OrderReplaceView v{m.body};
        const std::uint16_t* slot = route.find(v.original_ref());
        if (!slot) { ++unroutable; break; }
        const std::uint16_t s = *slot;
        if (s) {
          targets[s - 1].events.push_back(BookEvent{
              v.timestamp(), v.original_ref(), v.new_ref(), v.price(), v.shares(),
              EventType::Replace, Side::Buy, 0});
          ++routed;
        }
        route.erase(v.original_ref());
        route.insert(v.new_ref(), s);
        break;
      }
      default: break;
    }
  }

  std::printf("messages     : %" PRIu64 "\n", reader.stats().messages);
  std::printf("routed events: %" PRIu64 "\n", routed);
  std::printf("unroutable   : %" PRIu64 "  (references to orders added before this slice)\n",
              unroutable);

  int rc = 0;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const Target& tg = targets[i];
    if (tg.locate == 0) {
      std::fprintf(stderr, "WARNING: symbol %s never appeared in a Stock Directory message\n",
                   want[i].c_str());
      rc = 1;
      continue;
    }
    const std::string out = outdir + "/" + want[i] + ".tape";
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    TapeHeader h{};
    std::memcpy(h.magic, kTapeMagic, sizeof h.magic);
    h.version = kTapeVersion;
    h.event_size = sizeof(BookEvent);
    h.event_count = tg.events.size();
    h.symbol_raw = tg.symbol.raw;
    h.stock_locate = tg.locate;
    std::fwrite(&h, sizeof h, 1, f);
    if (!tg.events.empty()) std::fwrite(tg.events.data(), sizeof(BookEvent), tg.events.size(), f);
    std::fclose(f);
    std::printf("%-8s locate=%-6u events=%-12zu -> %s\n",
                want[i].c_str(), tg.locate, tg.events.size(), out.c_str());
  }
  return rc;
}
