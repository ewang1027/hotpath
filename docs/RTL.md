# The ITCH parser in SystemVerilog

`rtl/itch_parse.sv` is a byte-serial NASDAQ TotalView-ITCH 5.0 parser: framing
plus the seven message types that mutate an order book (A F E C X D U). It is
co-simulated against the C++ parser in `include/hotpath/itch/` under Verilator,
and the gate is that every decoded field matches on the same input.

```
./scripts/cosim.sh                        # synthetic + 5000 fuzzed inputs
FUZZ=20000 ./scripts/cosim.sh 60000000    # plus 60 MB of real NASDAQ data
```

The script rebuilds from scratch whenever a source is newer than the binary.
Verilator's incremental build did not reliably pick up an edited `.sv` here, and
a validation run that silently exercises the *previous* RTL will report PASS on
code that no longer exists — which happened once during this work and is worth
more caution than the three seconds it costs.

## Result

```
one of each type       7 messages, every field identical
4000 random messages   4000 messages, every field identical
malformed input        rtl e_short=2, rtl decoded=2, c++ decoded=2
RTL fuzz               20000 inputs, 1865896 bytes, 65204 messages, 693 rejected, 0 mismatches
real NASDAQ ITCH       2057603 messages, every field identical
CO-SIMULATION: PASS
```

**2,057,603 real messages decoded identically by two independent
implementations, one of them hardware**, plus 65,204 more from 20,000 fuzzed
inputs. Lint is clean under `-Wall`, and the assertions below are enabled
throughout.

## Differential fuzzing against the golden model

The generators are the same ones the software fuzz targets use
(`tests/fuzz_core.hpp`), pointed at the hardware. This is what a verification
team does with a reference model, and it is stronger than fuzzing either side
alone: a shared misreading of the spec cannot hide, because the two
implementations were written in different languages against different mental
models of the same document.

Both sides must agree on what they **rejected**, not merely on what they
accepted. A parser that quietly emits an under-length message is precisely the
bug that produced two heap-buffer-overflows on the C++ side, and comparing only
successful decodes would miss it entirely. The run above rejects 693 inputs and
the two agree on every one.

One design point the comparison forced: the two stop for *different reasons* on
a malformed stream — C++ halts at a zero-length frame, the RTL resynchronises
past it. That is a difference in recovery policy, not in decoding, so the
comparison is made over the prefix the reference actually consumed. Conflating
the two would have produced a stream of false mismatches that say nothing about
whether the fields decode correctly.

**The fuzzer was validated against a real bug**, the same discipline the
software campaign uses: reverting the per-type length check makes it fail within
85 inputs, with the RTL emitting messages the C++ side rejects as short.
Restoring it passes.

## Assertions

Output comparison catches wrong answers. Assertions catch a wrong *internal
state* that happens to produce a right answer — the failure mode that survives
a differential test and reappears later on different input.

| assertion | what it pins |
|---|---|
| `a_cnt_in_range` | the byte counter never runs past the body it is counting through |
| `a_valid_implies_long_enough` | nothing is emitted that was too short to contain what it claims |
| `a_valid_xor_short` | a message is either decoded or rejected, never both |
| `a_valid_one_cycle` / `a_short_one_cycle` | every strobe is exactly one cycle, so a consumer latching on a level cannot double-count |
| `a_emit_resolves` | the emit arm always resolves next cycle and cannot linger to re-fire against a later message's accumulators |

Built with `--assert -DHOTPATH_ASSERT`; none fired across the full run above.

This is the same argument the four order book designs make — two independent
implementations agreeing over a large input is evidence that neither is wrong in
an interesting way — except that here one is RTL and the other is C++, which
rules out a whole class of shared-assumption bugs that two C++ implementations
could still have in common.

## Why byte-serial is not a shortcut

At 30.7 bytes per message (8.25 GB over 268,744,780 messages), a 200 MHz
byte-serial pipe sustains **~6.5 M msg/s** — comfortably above what the real
feed delivers. Widening the datapath to 8 bytes/cycle, which is what a 10G feed
handler actually does, would multiply that by eight and change nothing about the
part that matters.

Because throughput is not what hardware buys here. What it buys is that the
decode completes a **fixed number of cycles** after the last byte arrives —
every message, every time, with no cache, no branch predictor, no scheduler and
no other tenant on the machine. That is precisely the tail behaviour the
software side of this repo cannot even measure: `METHODOLOGY.md` explains why a
41.7 ns clock makes a p99.9 tick-to-trade figure unmeasurable here, and an FPGA
sidesteps the question by not having a tail in the first place.

**Decode latency: 1 cycle after the final byte of a message.** Not "about one" —
exactly one, deterministically, and the co-simulation would fail if it were not.

## Three bugs the co-simulation caught

Both are RTL-specific, and both would have been extremely unpleasant to find any
other way.

### Reading an accumulator in the cycle that fills it

The first version emitted the decoded message on the same cycle as the last
byte:

```systemverilog
if (cnt >= 32 && cnt <= 35) acc_p32 <= {acc_p32[23:0], s_byte};
...
if (cnt == len - 1) m_price <= acc_p32;     // WRONG
```

Nonblocking assignments update at the end of the timestep, so `m_price` gets the
value from *before* the final byte was shifted in. Every price and size came out
short by one byte. The co-simulation flagged it on the first message; in
isolation this looks like a byte-order problem and gets "fixed" in the wrong
place. The parser now emits one cycle after the last byte, which is where the
deterministic 1-cycle latency above comes from.

### Validating only the common header, not the per-type length

The RTL initially rejected only bodies shorter than the 11-byte common header.
The C++ parser goes further and rejects a body shorter than its *type's* spec
length — a 20-byte message claiming to be an Add never receives bytes 32–35, so
its price would be whatever the accumulator last held.

The differential fuzzer found the disagreement within a hundred inputs. The RTL
now carries the same spec-length table and applies the same rule, so the two
reject identically rather than merely accepting identically.

### Emitting stale flops for fields the message does not have

A System Event is 12 bytes, so the byte counter never reaches the 11–18 range
that fills `acc_ref`. The accumulator therefore still held **the previous
message's order reference**, and the parser emitted it.

That is the RTL analogue of reading uninitialised memory, with one important
difference: stale flops are invisible to every sanitizer that exists. There is
no ASan for a register that was never written this message. Only a differential
check against an implementation that knows the field does not exist will find
it. Every field is now gated on the type that actually carries it.

## Structure

Field offsets collide between message types — byte 19 is the side of an Add, the
first byte of the executed size of an Execute, and the first byte of the new
reference of a Replace — so each candidate gets its own small shift register and
the type selects between them at end-of-message:

| accumulator | bytes | used by |
|---|---|---|
| `acc_locate` | 1–2 | all |
| `acc_ts` | 5–10 | all |
| `acc_ref` | 11–18 | A F E C X D U |
| `acc_ref2` | 19–26 | U (new reference) |
| `acc_q19` | 19–22 | E C X (shares) |
| `acc_q20` | 20–23 | A F (shares) |
| `acc_q27` / `acc_p31` | 27–34 | U (shares, price) |
| `acc_p32` | 32–35 | A F (price) |

In hardware these are a few dozen flops and cost essentially nothing. Sharing
them would save flops and buy a mux on the critical path, which is the wrong
trade.

## Malformed input

The parser refuses to emit a message whose body is shorter than the 11-byte
common header, raising `e_short` instead. This is not defensive
over-engineering: the C++ parser had **two heap-buffer-overflows** from exactly
this omission (`BUILDLOG.md` phase 13), and the co-simulation checks that both
implementations reject the same inputs rather than merely agreeing on the ones
they accept.

## Not claimed

This is simulated, not synthesised. There is no place-and-route, no timing
closure at a stated frequency, and no FPGA. The 200 MHz figure above is an
assumption used to make a throughput argument, not a measured `Fmax`. What is
demonstrated is that the RTL is functionally correct against a reference on two
million real messages, which is the part that has to be true before any of the
rest is worth doing.
