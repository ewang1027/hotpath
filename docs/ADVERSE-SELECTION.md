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

## Honest limitations

- **No self-impact.** Our order is hypothetical and never enters the book, so it
  never deters or attracts anyone else's order. A real 100-share quote at the
  touch would change the flow it is measuring.
- **No latency.** Re-quotes are instantaneous. In reality the touch moves and
  you arrive late, which makes queue position *worse* than modelled — so the
  fill counts above are still an upper bound.
- **Queue-position-only fill priority.** Hidden orders, odd-lot handling, and
  non-displayed liquidity are not modelled; ITCH does not show them.
- **One day, four symbols.** The direction of every result is consistent across
  the four, but a single session is a single sample.
