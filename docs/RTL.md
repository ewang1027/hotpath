# The ITCH parser in SystemVerilog

`rtl/itch_parse.sv` is a byte-serial NASDAQ TotalView-ITCH 5.0 parser: framing
plus the seven message types that mutate an order book (A F E C X D U). It is
co-simulated against the C++ parser in `include/hotpath/itch/` under Verilator,
and the gate is that every decoded field matches on the same input.

```
./scripts/cosim.sh              # synthetic streams
./scripts/cosim.sh 60000000     # plus 60 MB of real NASDAQ data
```

## Result

```
one of each type       7 messages, every field identical
4000 random messages   4000 messages, every field identical
malformed input        rtl e_short=2, rtl decoded=2, c++ decoded=2
real NASDAQ ITCH       2057603 messages, every field identical
CO-SIMULATION: PASS
```

**2,057,603 real messages decoded identically by two independent
implementations, one of them hardware.** Lint is clean under `-Wall`.

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

## Two bugs the co-simulation caught

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
