# Does a signal that predicts price make a passive strategy better?

Short answer, measured on 25 symbols: **the signals predict, and acting on them
does not help.** This document is the negative result and the reason for it.

Reproduce with `./build/src/signal_study <SYM>.tape` and
`./build/src/strategy_eval <SYM>.tape`.

## 1. The signals do predict

Queue imbalance `(Qb − Qa)/(Qb + Qa)` at the touch, and order flow imbalance
(Cont–Kukanov–Stoikov) accumulated as a time-decayed sum. Sampled on a fixed
100 ms grid — *not* per event, because events arrive in bursts and per-event
sampling would weight busy microseconds hundreds of times more than quiet
seconds, turning autocorrelation into an apparent signal.

Correlation with the forward mid return:

| symbol | imbalance @100ms | imbalance @1s | OFI @1s |
|---|---:|---:|---:|
| AAPL | 0.051 | 0.060 | 0.078 |
| SPY | 0.020 | 0.045 | 0.009 |
| MSFT | 0.027 | 0.074 | 0.030 |
| INTC | 0.117 | **0.219** | 0.026 |
| AMZN | 0.034 | 0.053 | 0.081 |
| QQQ | 0.227 | **0.208** | 0.052 |
| F | 0.052 | 0.105 | 0.039 |
| SIRI | 0.084 | 0.118 | 0.018 |

Positive on every symbol, and the decile response is a clean monotone staircase
— which matters more than the correlation, because a single fat tail can
manufacture a correlation but not a staircase. AAPL at 1 s:

```
-0.107 -0.042 -0.040 -0.006 -0.017 +0.017 +0.027 +0.030 +0.063 +0.080   (bps)
```

Both signals are gone by 10 s (AAPL correlation 0.004). Imbalance is the more
robust of the two across symbols; OFI is stronger on AAPL and AMZN but nearly
absent on SPY.

Imbalance predicts best on tick-constrained names — INTC ($56) and QQQ ($212)
top the table — which is what you would expect. When the spread is pinned at one
tick, the queue sizes are the only thing left that can carry information.

### Microprice tilt is not a second signal

Stoikov's microprice, normalised by the half-spread, reduces algebraically to
the queue imbalance:

```
(microprice − mid) / (spread/2)
  = [ (Qb·Pa + Qa·Pb)/(Qb+Qa) − (Pa+Pb)/2 ] / ((Pa−Pb)/2)
  = (Qb − Qa)/(Qb + Qa)
```

Verified numerically over 200,000 random books (max deviation 1.7e-9) and pinned
by a test. They are one signal. Shipping both as separate features would be
double-counting a single piece of information — which the identical rows in the
first `signal_study` output made obvious.

## 2. Acting on it does not improve markout

Policy: when the book leans hard one way, quote only one side. Two directions,
run as exact mirrors of each other:

- **quote the HEAVY side** — what the price forecast argues for. A heavy bid
  predicts a rising price, so stop offering and stop being lifted.
- **quote the THIN side** — the same threshold, near-identical duty cycle,
  opposite direction.

The mirror is what makes the experiment readable. Pulling a quote forfeits queue
position *whichever* direction you pull it, so a one-sided policy can lose for
reasons that have nothing to do with the signal. Differencing the two mirrors
cancels that cost and leaves only the signal.

Comparison is a **paired block bootstrap** over 5-minute blocks, 2000 resamples.
Both policies trade the same market, so resampling the same blocks for both
cancels the shared variance; comparing two independent confidence intervals
would be a far weaker test.

**Result, 25 symbols, 1 s markout horizon, threshold 0.50:**

| | quote-THIN minus quote-HEAVY |
|---|---|
| positive (thin better) | 9 / 25 |
| negative (heavy better) | 16 / 25 |
| two-sided sign test | **p = 0.230** |

A coin flip. Individually "significant" results appear in *both* directions,
which is what multiple testing looks like when there is no effect. The same null
holds at the 10 s horizon (11/25 positive).

## 3. The false positive I would have shipped

On AAPL alone, at threshold 0.50, quoting the thin side beat quoting the heavy
side by **+0.101 bps with a paired 95% CI of [+0.001, +0.208]** — significant at
p < 0.05, with a tidy mechanical story attached about how being filled is itself
informative and reverses the sign of the forecast.

It does not replicate. Testing 25 symbols at the 5% level should produce roughly
one false positive by chance, and that is what this was. Had the study stopped
at the first symbol — which is where a result this clean invites you to stop —
the repo would be reporting an edge that does not exist.

The only reason it was caught is that the cross-sectional run was written before
the number was looked at.

## 4. Why the signal does not convert

The forecast is real but smaller than the cost of acting on it.

Comparing the signal's **entire top-to-bottom decile range** at 1 s against a
single half-spread:

| symbol | half-spread (bps) | signal range (bps) | ratio |
|---|---:|---:|---:|
| QQQ | 0.298 | 0.292 | 0.98 |
| INTC | 0.955 | 0.700 | 0.73 |
| SPY | 0.212 | 0.151 | 0.71 |
| MSFT | 0.493 | 0.268 | 0.54 |
| AAPL | 0.584 | 0.187 | 0.32 |
| F | 5.856 | 0.613 | 0.10 |
| SIRI | 7.711 | 0.716 | 0.09 |
| AMZN | 1.718 | 0.124 | 0.07 |

The ratio never reaches 1. Moving from the *most bearish decile to the most
bullish* — the largest swing the signal ever offers — is worth less than one
half-spread everywhere, and less than a tenth of one on the wide-spread names.
A realistic gating decision uses a fraction of that range.

Against that, acting costs:

- **queue position**, forfeited on every pull and rebuilt from the back;
- **foregone fills** on the suppressed side, which were not all toxic.

A signal has to beat the cost of trading on it, and this one does not. That is a
different and more useful statement than "the signal doesn't work" — the signal
works fine as a *forecast*. It just isn't worth a half-spread.

## 5. What the two fill models say

Both models were run on identical strategy code, signal, threshold and latency;
only the fill rule differed.

- They agree on the sign of the effect on **23 of 25** symbols (disagreeing on
  GE and JPM, both thin books with few fills).
- The naive model reports the **larger magnitude on 17 of 25**.
- Neither finds a robust effect, and they agree there is none.

So on this experiment the fill model changes the *size* of a measured effect
more than its direction. That is a weaker claim than the one I set out to test —
that the naive model would reverse a verdict — and it is reported as measured
rather than as hoped.

## Limitations

- One trading day. The cross-section is 25 symbols wide; the time series is n=1,
  and a sign test across correlated symbols on a single day is not 25
  independent trials.
- One policy shape. Gating quotes on and off is crude; skewing prices by a tick
  or sizing on the signal would forfeit less queue position and might convert
  where this does not.
- The markout horizon is fixed at 1 s (10 s reported alongside), matched to
  where the signal demonstrably has power.
- No self-impact, as everywhere else in this repo.
