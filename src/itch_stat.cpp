// Day-1 gate: parse a full ITCH 5.0 trading day and prove the parse is sound.
//
// Three claims are checked here, and all three are pass/fail:
//   1. Framing   -- every message's 2-byte length prefix agrees with the spec
//                   length for its type, and the stream consumes to EOF with
//                   no truncation.
//   2. Referential integrity -- every execute/cancel/delete/replace names an
//                   order reference that is currently live. An orphan means we
//                   mis-parsed an offset somewhere upstream.
//   3. Zero allocation -- the steady-state loop allocates nothing and issues
//                   no syscalls. Reported, not assumed.
#include "hotpath/bench/counters.hpp"
#include "hotpath/bench/timing.hpp"
#include "hotpath/core/open_hash.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/itch/mapped_file.hpp"
#include "hotpath/itch/reader.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace hotpath;
using namespace hotpath::itch;

namespace {

struct LiveOrder {
  Qty shares;
};

struct Integrity {
  std::uint64_t orphan_execute{0};
  std::uint64_t orphan_cancel{0};
  std::uint64_t orphan_delete{0};
  std::uint64_t orphan_replace{0};
  std::uint64_t oversized_execute{0};   // executed more than was resting
  std::uint64_t oversized_cancel{0};
  std::uint64_t duplicate_add{0};
  std::uint64_t table_full{0};
  std::uint64_t peak_live{0};
};

const char* type_name(char t) {
  switch (t) {
    case 'S': return "SystemEvent";        case 'R': return "StockDirectory";
    case 'H': return "TradingAction";      case 'Y': return "RegSHO";
    case 'L': return "ParticipantPos";     case 'V': return "MwcbDecline";
    case 'W': return "MwcbStatus";         case 'K': return "IpoQuoting";
    case 'J': return "LuldCollar";         case 'h': return "OperationalHalt";
    case 'A': return "AddOrder";           case 'F': return "AddOrderMPID";
    case 'E': return "OrderExecuted";      case 'C': return "OrderExecPrice";
    case 'X': return "OrderCancel";        case 'D': return "OrderDelete";
    case 'U': return "OrderReplace";       case 'P': return "TradeNonCross";
    case 'Q': return "CrossTrade";         case 'B': return "BrokenTrade";
    case 'I': return "NOII";               case 'N': return "RPII";
    default:  return "?";
  }
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  std::uint64_t limit = 0;              // 0 = whole file
  std::size_t table_pow2 = 1u << 24;    // 16.7M slots

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--limit" && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--table" && i + 1 < argc) table_pow2 = std::strtoull(argv[++i], nullptr, 10);
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr,
        "usage: itch_stat <file.NASDAQ_ITCH50> [--limit N] [--table SLOTS]\n");
    return 2;
  }

  request_performance_cores();

  MappedFile file(path);
  std::printf("file      : %s\n", path.c_str());
  std::printf("size      : %.2f GiB\n",
              static_cast<double>(file.size()) / (1024.0 * 1024.0 * 1024.0));

  // Everything the loop touches is allocated here, before the counters are
  // snapshotted. Anything the loop allocates shows up as a violation.
  OpenHashMap<LiveOrder> live(table_pow2);
  Integrity integ{};
  Reader reader(file.data(), file.size());
  RawMessage msg{};

  bench::CounterScope counters;
  const std::uint64_t t0 = now_ticks();

  std::uint64_t n = 0;
  while (reader.next(msg)) {
    const char t = msg.type();
    switch (t) {
      case 'A':
      case 'F': {
        const AddOrderView v{msg.body};
        if (live.find(v.order_ref())) ++integ.duplicate_add;
        if (!live.insert(v.order_ref(), LiveOrder{v.shares()})) ++integ.table_full;
        if (live.size() > integ.peak_live) integ.peak_live = live.size();
        break;
      }
      case 'E':
      case 'C': {
        const OrderExecutedView v{msg.body};
        if (LiveOrder* o = live.find(v.order_ref())) {
          if (v.executed_shares() > o->shares) ++integ.oversized_execute;
          else if ((o->shares -= v.executed_shares()) == 0) live.erase(v.order_ref());
        } else ++integ.orphan_execute;
        break;
      }
      case 'X': {
        const OrderCancelView v{msg.body};
        if (LiveOrder* o = live.find(v.order_ref())) {
          if (v.cancelled_shares() > o->shares) ++integ.oversized_cancel;
          else if ((o->shares -= v.cancelled_shares()) == 0) live.erase(v.order_ref());
        } else ++integ.orphan_cancel;
        break;
      }
      case 'D': {
        const OrderDeleteView v{msg.body};
        if (!live.erase(v.order_ref())) ++integ.orphan_delete;
        break;
      }
      case 'U': {
        const OrderReplaceView v{msg.body};
        if (!live.erase(v.original_ref())) ++integ.orphan_replace;
        if (!live.insert(v.new_ref(), LiveOrder{v.shares()})) ++integ.table_full;
        if (live.size() > integ.peak_live) integ.peak_live = live.size();
        break;
      }
      default: break;
    }
    if (limit && ++n >= limit) break;
  }

  const std::uint64_t elapsed = now_ticks() - t0;
  const auto d = counters.delta();
  const auto& st = reader.stats();

  const double secs = ticks_to_ns(elapsed) / 1e9;
  const bench::Throughput tp{st.messages, secs};

  std::printf("\n-- framing --\n");
  std::printf("messages          : %" PRIu64 "\n", st.messages);
  std::printf("bytes consumed    : %" PRIu64 " / %zu\n", st.bytes, file.size());
  std::printf("unknown types     : %" PRIu64 "\n", st.unknown_type);
  std::printf("length mismatches : %" PRIu64 "\n", st.length_mismatch);
  std::printf("truncated tail    : %" PRIu64 "\n", st.truncated);

  std::printf("\n-- message mix --\n");
  for (int t = 0; t < 256; ++t) {
    if (!st.per_type[t]) continue;
    std::printf("  %c %-16s %12" PRIu64 "  %5.2f%%\n", t ? char(t) : '?',
                type_name(char(t)), st.per_type[t],
                100.0 * static_cast<double>(st.per_type[t]) /
                    static_cast<double>(st.messages ? st.messages : 1));
  }

  std::printf("\n-- referential integrity --\n");
  std::printf("orphan execute    : %" PRIu64 "\n", integ.orphan_execute);
  std::printf("orphan cancel     : %" PRIu64 "\n", integ.orphan_cancel);
  std::printf("orphan delete     : %" PRIu64 "\n", integ.orphan_delete);
  std::printf("orphan replace    : %" PRIu64 "\n", integ.orphan_replace);
  std::printf("oversized execute : %" PRIu64 "\n", integ.oversized_execute);
  std::printf("oversized cancel  : %" PRIu64 "\n", integ.oversized_cancel);
  std::printf("duplicate add     : %" PRIu64 "\n", integ.duplicate_add);
  std::printf("table full        : %" PRIu64 "\n", integ.table_full);
  std::printf("peak live orders  : %" PRIu64 " (load %.3f of %zu slots)\n",
              integ.peak_live,
              static_cast<double>(integ.peak_live) / static_cast<double>(table_pow2),
              table_pow2);
  std::printf("still live at EOD : %zu\n", live.size());

  std::printf("\n-- steady-state invariants --\n");
  std::printf("allocations       : %" PRIu64 "  %s\n", d.allocations,
              d.allocations == 0 ? "[OK]" : "[VIOLATION]");
  std::printf("syscalls          : %" PRIu64 "  %s\n", d.syscalls,
              d.syscalls == 0 ? "[OK]" : "[VIOLATION]");

  std::printf("\n-- throughput (amortised; see docs/METHODOLOGY.md) --\n");
  std::printf("elapsed           : %.3f s\n", secs);
  std::printf("rate              : %.2f M msg/s\n", tp.msgs_per_sec() / 1e6);
  std::printf("amortised         : %.1f ns/msg\n", tp.ns_per_msg());
  std::printf("ingest            : %.2f GiB/s\n",
              static_cast<double>(st.bytes) / secs / (1024.0 * 1024.0 * 1024.0));

  const bool pass = st.length_mismatch == 0 && st.truncated == 0 &&
                    integ.orphan_execute == 0 && integ.orphan_cancel == 0 &&
                    integ.orphan_delete == 0 && integ.orphan_replace == 0 &&
                    integ.table_full == 0 && d.allocations == 0 && d.syscalls == 0;
  std::printf("\nGATE: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
