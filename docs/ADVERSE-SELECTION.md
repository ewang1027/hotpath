# Queue position, fills, and adverse selection

One NASDAQ ITCH 5.0 trading day (2019-12-30). A passive market maker joins the
touch on both sides with a 100-share quote and re-quotes whenever the touch
moves. Reproduce with `./build/src/sim_mm <SYM>.tape --size 100`.

## Why the fill model is the whole point

The simulated strategy here is deliberately trivial — join the touch, re-quote
when it moves. Nothing about it is clever, and it is not supposed to be. The
question this answers is not "is this a good strategy" but **"how wrong is the
fill model that most backtests use?"**

Two models run side by side on identical order flow:

- **naive** — you fill whenever the price trades, up to your quoted size. This is
  what a backtest does implicitly when it has quotes and trade prints and no
  order-by-order book.
- **queue-aware** — you fill only once cumulative executed volume at your price
  exceeds the volume resting *ahead* of you, and your position in the queue also
  advances when orders ahead of you are **cancelled**.

That second clause matters more than it sounds. On this day, adds and deletes
are 85% of all messages and executions are 1.7%: **most of the queue in front of
you disappears by being cancelled, not by trading.** A model that only advances
the queue on executions concludes you never get to the front. A model that fills
whenever the price trades concludes you are always at the front. Both are wrong,
in opposite directions.

## Result 1 — the naive model overstates fills by 3.7x to 6.4x

| symbol | naive fills | naive shares | queue-aware fills | queue-aware shares | overstatement |
|---|---:|---:|---:|---:|---:|
| AAPL | 60,543 | 3,523,775 | 9,535 | 548,567 | **6.4x** |
| SPY | 42,635 | 3,114,511 | 12,058 | 840,427 | **3.7x** |
| MSFT | 35,315 | 2,231,187 | 9,403 | 597,638 | **3.7x** |
| INTC | 19,405 | 1,376,264 | 4,434 | 290,994 | **4.7x** |

A backtest using the naive model books between 3.7 and 6.4 times the passive
volume it would actually get. Every per-share edge it computes is being applied
to a fill count that does not exist.

## Result 2 — passive fills are adversely selected

Markout is the mid-price move after the fill, signed so that positive means the
fill was good: `sign × (mid(t+h) − fill_price)`, with `sign = +1` for buys.

Queue-aware model, mean markout in basis points:

| symbol | 1s | 10s | 60s | 10s buys | 10s sells |
|---|---:|---:|---:|---:|---:|
| AAPL | −0.220 | −0.282 | −0.307 | −0.440 | −0.123 |
| MSFT | −0.096 | −0.165 | −0.199 | −0.172 | −0.156 |
| INTC | −0.365 | −0.393 | −0.412 | −0.499 | −0.298 |
| SPY | −0.035 | −0.038 | **+0.027** | −0.036 | −0.039 |

Markouts are negative almost everywhere and get worse as the horizon lengthens:
after you are filled, the mid keeps moving against you. That is adverse
selection — the counterparty who traded with you knew something, or at minimum
your fill was caused by the price moving through your level.

SPY is the informative exception. It is the most liquid instrument here and its
adverse selection is an order of magnitude smaller (−0.038 bps at 10s versus
AAPL's −0.282), turning slightly positive by 60s. That is what you would expect
from a broad-index ETF: order flow in it carries far less single-name private
information than flow in an individual stock. The measurement recovers a
well-known microstructure fact without being told about it.

## Result 3 — the deeper your queue position, the worse your fills

10-second markout on the queue-aware model, bucketed by how much volume was
resting ahead when we joined:

| volume ahead at join | AAPL | SPY | MSFT | INTC |
|---|---:|---:|---:|---:|
| 1–100 | −0.205 | 0.000 | −0.103 | −0.460 |
| 101–500 | −0.388 | −0.014 | −0.143 | −0.210 |
| 501–2000 | **−0.813** | −0.123 | **−0.333** | −0.449 |
| >2000 | +0.111 *(64 fills)* | −0.036 | −0.270 | −0.479 |

On AAPL the markout worsens monotonically with depth, roughly **4x worse** from
the shallowest bucket to the 501–2000 bucket. MSFT and SPY show the same
direction; INTC is noisier.

The mechanism is mechanical once stated: to be filled from deep in a queue, the
market has to trade *through* everything resting in front of you at that price.
That only happens when the price is being pushed through your level — which is
precisely the case where you did not want the fill. **Shallow-queue fills are
mostly noise trades; deep-queue fills are disproportionately informed ones.**

This is why queue position is a first-class quantity in real passive trading and
not an implementation detail: it changes both *how often* you fill and *which*
fills you get.

## Relation to the theory

This is the empirical counterpart to the Glosten–Milgrom work in
[`simplified_gto_solver`](https://github.com/ewang1027/simplified_gto_solver),
where adverse selection is *solved for* in a game where a maker quotes against
possibly-informed flow. Here it is *measured*, from one day of real order flow,
with no model of the informed trader at all — and it shows up as a negative
markout that scales with how much the counterparty had to trade through to reach
you.

## Replication across 25 symbols

Results 1–3 were first measured on four symbols. Four points can describe almost
any curve, so the study was widened to 25 spanning $7 (SIRI) to $1,784 (AMZN)
and 70K to 2.4M events — 18,798,869 events in total, all four book designs
cross-validating with **zero divergences** on every one.

| finding | replication |
|---|---|
| Naive model overstates filled volume | **25 / 25**, from 3.4x to **15.6x**, median 5.7x |
| Passive fills are adversely selected (negative 10s markout) | **25 / 25** |
| Markout worsens with queue depth | **10 / 13** symbols with ≥200 fills in both buckets |

**The overstatement is worse than the four-symbol sample suggested.** The
original range was 3.7–6.4x; across 25 it reaches **15.6x on SIRI** and 12.9x on
AMZN. The pattern is systematic: the overstatement is largest exactly where you
fill least. SPY (12,058 fills) and MSFT (9,403) sit at 3.7x, while SIRI (82
fills) and GE (291) are at 15.6x and 12.8x. Where a passive order genuinely
reaches the front of the queue, the two models converge; where it rarely does,
assuming it always fills is catastrophically wrong.

**The queue-depth result is real but weaker than four symbols implied.** It
holds on 10 of the 13 symbols with enough fills in both buckets to compare
(AAPL, AMD, PFE, WFC, KO, MSFT, SPY, IWM, QQQ, XOM) and fails on three (CSCO,
INTC, JPM). Reported as a directional effect, not a law. The 12 remaining
symbols have too few deep-queue fills — often fewer than 100 — to say anything,
and are excluded rather than counted as support.

Where it does hold it can be large: AMZN's deep bucket is −4.92 bps against
−0.89 shallow, and TSLA's is −2.73 against −0.79, though both rest on fewer than
100 deep fills.

## Result 4 — what re-quote latency actually costs

Everything above assumes re-quotes are instantaneous. They are not, and the
delay between seeing the touch move and having your quote in the right place is
the single number every nanosecond of tick-to-trade engineering exists to
reduce. `latency_sweep` measures what it buys.

Two mechanisms pull in opposite directions:

1. **You join later**, so more participants are ahead of you. Fewer fills.
2. **Your old quote is still resting at a stale price** while the replacement is
   in flight. When the market moves away, that stale quote is now the most
   aggressive one in the book and gets run over. *More* fills — and precisely
   the ones you did not want.

Mean 10s markout in bps, by re-quote latency:

| latency | AAPL | SPY | MSFT | INTC |
|---|---:|---:|---:|---:|
| 0 | −0.259 | −0.011 | −0.180 | −0.422 |
| 1 µs | −0.251 | −0.013 | −0.181 | −0.414 |
| 10 µs | −0.256 | −0.021 | −0.194 | −0.449 |
| 50 µs | −0.300 | −0.030 | −0.229 | −0.540 |
| 100 µs | −0.304 | −0.043 | −0.254 | −0.531 |
| 1 ms | −0.352 | −0.054 | −0.275 | −0.553 |
| 10 ms | **−0.406** | **−0.072** | **−0.319** | **−0.565** |

**Fill quality degrades monotonically with latency on every symbol** — 57% worse
on AAPL, 77% on MSFT, and 6.5x on SPY, which starts closest to break-even and
therefore has the most to lose in relative terms.

### The counterintuitive part: latency does not simply cost you fills

| | AAPL fills | stale share of volume | swept fills |
|---|---:|---:|---:|
| 0 | 9,535 | 0.0% | 0 |
| 1 µs | 10,518 | 10.2% | 989 |
| 100 µs | 9,456 | 16.5% | 1,255 |
| 10 ms | **11,014** | **50.0%** | 4,501 |

On AAPL the fill count is *U-shaped*: it rises at microsecond latency, dips in
the middle, and rises again past a millisecond — ending 15% **above** the
zero-latency count. By 10 ms, **half of all filled volume is on stale quotes**.

So the naive intuition ("slow means you miss fills") is wrong on the most active
symbol. Latency does not reduce how much you trade so much as it changes *what*
you trade: you lose the queue races you wanted to win and get filled on the
quotes you were trying to cancel. SPY, MSFT and INTC do show declining fill
counts, but their markouts degrade all the same — the mix shifts even when the
volume falls.

### Why microseconds register at all

The median gap between events on AAPL is 105 µs, which makes a 1 µs delay look
irrelevant. It is not, because **executions arrive in bursts far tighter than
the average**:

| symbol | median inter-event gap | median gap preceding an *execution* | gaps < 1 µs |
|---|---:|---:|---:|
| AAPL | 105 µs | **26 µs** | 5.0% |
| SPY | 108 µs | **6.3 µs** | 4.1% |

Fills happen during bursts, and during bursts the market moves in microseconds.
Latency bites exactly when it matters. Regenerate with
`./build/src/tape_stat <SYM>.tape`.

### A caveat on the dollar figures

`latency_sweep` also prints a P&L proxy (markout × filled shares). Read the
markout column, not that one. This strategy has *negative* edge at every latency
— it quotes at the touch unconditionally, with no inventory management, no
skew, and no toxicity filter — so "trade less" mechanically improves P&L. On
INTC, higher latency therefore looks *better* on the dollar axis while fill
quality is plainly getting worse. Per-share markout is the honest metric for a
strategy that should not be trading in the first place.

## Honest limitations

- **No self-impact.** Our order is hypothetical and never enters the book, so it
  never deters or attracts anyone else's order. A real 100-share quote at the
  touch would change the flow it is measuring.
- **The strategy is deliberately trivial.** Join the touch, re-quote when it
  moves. It is a measurement instrument for the fill model, not a proposal.
- **Latency is modelled as a fixed delay**, symmetric for cancels and new
  orders, with no queueing or jitter.
- **Queue-position-only fill priority.** Hidden orders, odd-lot handling, and
  non-displayed liquidity are not modelled; ITCH does not show them.
- **One day, four symbols.** The direction of every result is consistent across
  the four, but a single session is a single sample.
