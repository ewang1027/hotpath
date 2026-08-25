// NASDAQ TotalView-ITCH 5.0 message parser, byte-serial.
//
// Consumes one byte per accepted clock and emits a decoded message on a
// single-cycle strobe. Scope is the framing plus the seven message types that
// mutate an order book (A F E C X D U), which is what a feed handler actually
// has to decode at line rate; the rest are reference data and can be handled
// far from the critical path.
//
// Why byte-serial is not a cop-out here: at 31 bytes per message on average, a
// 200 MHz byte-serial pipe sustains ~6.5 M msg/s, and real ITCH peaks in the
// low millions. Throughput is not the interesting number. What hardware buys is
// that the decode completes a FIXED small number of cycles after the last byte
// arrives, every time, with no cache, no branch predictor and no scheduler in
// the way -- which is precisely the tail behaviour the software side of this
// repo cannot even measure (see docs/METHODOLOGY.md).
//
// Verified against the C++ implementation in include/hotpath/itch/ by feeding
// both the same byte stream and comparing every decoded field. See
// rtl/tb_itch_parse.cpp.

`default_nettype none

module itch_parse (
    input  wire         clk,
    input  wire         rst,

    // Byte stream in. One byte is consumed on every cycle s_valid is high.
    input  wire         s_valid,
    input  wire [7:0]   s_byte,

    // Decoded message out, asserted for exactly one cycle per message.
    output logic        m_valid,
    output logic [7:0]  m_type,
    output logic [15:0] m_length,
    output logic [47:0] m_timestamp,
    output logic [15:0] m_locate,
    output logic [63:0] m_order_ref,
    output logic [63:0] m_new_ref,     // Order Replace only
    output logic [31:0] m_shares,
    output logic [31:0] m_price,
    output logic        m_side_buy,    // Add Order only
    output logic        m_book_event,  // type is one of A F E C X D U

    // Malformed-input strobes. A parser that silently accepts these is how the
    // software version ended up with two out-of-bounds reads (docs/BUILDLOG.md).
    output logic        e_short,       // body shorter than the 11-byte header
    output logic        e_zero_len     // zero-length frame: end of stream
);

  typedef enum logic [1:0] { S_LEN_HI, S_LEN_LO, S_BODY } state_t;
  state_t state;

  localparam int HEADER_LEN = 11;

  logic [15:0] len;      // body length from the frame prefix
  logic [15:0] cnt;      // byte offset within the body

  // The decode is emitted one cycle AFTER the final byte, not on it.
  //
  // Reading an accumulator in the same always_ff that shifts the last byte into
  // it yields the value from before that byte -- nonblocking assignments update
  // at the end of the timestep. The first version did exactly that and decoded
  // every price and size short by one byte. The co-simulation caught it on the
  // first message; in isolation it is the kind of bug that looks like a
  // byte-order problem and gets "fixed" in the wrong place.
  logic        emit;
  logic [15:0] emit_len;
  logic        emit_short;

  // Field accumulators. Offsets collide between types -- byte 19 is the side
  // for an Add, the first byte of the executed size for an Execute, and the
  // first byte of the new reference for a Replace -- so each candidate field
  // gets its own shift register and the type selects between them at
  // end-of-message. In hardware these are a few dozen flops and cost nothing;
  // trying to share them would cost a mux on the critical path.
  logic [47:0] acc_ts;      // bytes  5..10  timestamp
  logic [15:0] acc_locate;  // bytes  1..2   stock locate
  logic [63:0] acc_ref;     // bytes 11..18  order reference
  logic [63:0] acc_ref2;    // bytes 19..26  Replace: new reference
  logic [31:0] acc_q19;     // bytes 19..22  Execute/Cancel: shares
  logic [31:0] acc_q20;     // bytes 20..23  Add: shares
  logic [31:0] acc_q27;     // bytes 27..30  Replace: shares
  logic [31:0] acc_p31;     // bytes 31..34  Replace: price
  logic [31:0] acc_p32;     // bytes 32..35  Add: price
  logic        acc_side;

  wire [7:0] t = m_type;
  wire is_add     = (t == "A") || (t == "F");
  wire is_exec    = (t == "E") || (t == "C");
  wire is_cancel  = (t == "X");
  wire is_delete  = (t == "D");
  wire is_replace = (t == "U");
  wire is_book    = is_add | is_exec | is_cancel | is_delete | is_replace;

  always_ff @(posedge clk) begin
    if (rst) begin
      state      <= S_LEN_HI;
      m_valid    <= 1'b0;
      e_short    <= 1'b0;
      e_zero_len <= 1'b0;
      cnt        <= '0;
      len        <= '0;
      emit       <= 1'b0;
    end else begin
      // Strobes are single-cycle by construction.
      m_valid    <= 1'b0;
      e_short    <= 1'b0;
      e_zero_len <= 1'b0;

      if (s_valid) begin
        unique case (state)
          S_LEN_HI: begin
            len[15:8] <= s_byte;
            state     <= S_LEN_LO;
          end

          S_LEN_LO: begin
            len[7:0] <= s_byte;
            cnt      <= '0;
            if ({len[15:8], s_byte} == 16'd0) begin
              // Zero-length frame terminates the stream. Reported rather than
              // treated as a clean end: mid-file it means corruption.
              e_zero_len <= 1'b1;
              state      <= S_LEN_HI;
            end else begin
              state <= S_BODY;
            end
          end

          S_BODY: begin
            if (cnt == 16'd0) m_type <= s_byte;

            // Capture every candidate field unconditionally; select by type at
            // the end. Each is a plain shift register, so this is a handful of
            // enables rather than a wide mux in the datapath.
            if (cnt >= 16'd1  && cnt <= 16'd2 ) acc_locate <= {acc_locate[7:0],  s_byte};
            if (cnt >= 16'd5  && cnt <= 16'd10) acc_ts     <= {acc_ts[39:0],     s_byte};
            if (cnt >= 16'd11 && cnt <= 16'd18) acc_ref    <= {acc_ref[55:0],    s_byte};
            if (cnt >= 16'd19 && cnt <= 16'd26) acc_ref2   <= {acc_ref2[55:0],   s_byte};
            if (cnt >= 16'd19 && cnt <= 16'd22) acc_q19    <= {acc_q19[23:0],    s_byte};
            if (cnt >= 16'd20 && cnt <= 16'd23) acc_q20    <= {acc_q20[23:0],    s_byte};
            if (cnt >= 16'd27 && cnt <= 16'd30) acc_q27    <= {acc_q27[23:0],    s_byte};
            if (cnt >= 16'd31 && cnt <= 16'd34) acc_p31    <= {acc_p31[23:0],    s_byte};
            if (cnt >= 16'd32 && cnt <= 16'd35) acc_p32    <= {acc_p32[23:0],    s_byte};
            if (cnt == 16'd19) acc_side <= (s_byte == "B");

            if (cnt == len - 16'd1) begin
              // Arm the emit; the accumulators are complete next cycle.
              state      <= S_LEN_HI;
              emit       <= 1'b1;
              emit_len   <= len;
              emit_short <= (len < HEADER_LEN[15:0]);
            end
            cnt <= cnt + 16'd1;
          end
        endcase
      end

      // Emit fires regardless of s_valid: the last byte has already been
      // accepted, and holding the result hostage to the next byte arriving
      // would make the latency depend on the gap between messages.
      if (emit) begin
        emit <= 1'b0;
        if (emit_short) begin
          // Too short to contain the common header. Emitting it would hand
          // downstream logic fields that were never received -- the same
          // mistake that produced two out-of-bounds reads on the C++ side.
          e_short <= 1'b1;
        end else begin
          m_valid      <= 1'b1;
          m_length     <= emit_len;
          m_timestamp  <= acc_ts;
          m_locate     <= acc_locate;
          // Every field is gated on the type that actually carries it. A
          // 12-byte System Event never shifts bytes 11..18 in, so acc_ref still
          // holds whatever the PREVIOUS message left there -- and emitting it
          // would hand downstream logic a plausible-looking order reference
          // that was never received. This is the RTL analogue of reading
          // uninitialised memory, except that stale flops are invisible to
          // every sanitizer. The co-simulation is what found it.
          m_order_ref  <= is_book    ? acc_ref  : 64'd0;
          m_new_ref    <= is_replace ? acc_ref2 : 64'd0;
          m_side_buy   <= is_add ? acc_side : 1'b0;
          m_book_event <= is_book;
          m_shares     <= is_add     ? acc_q20
                        : (is_exec | is_cancel) ? acc_q19
                        : is_replace ? acc_q27
                        : 32'd0;
          m_price      <= is_add     ? acc_p32
                        : is_replace ? acc_p31
                        : 32'd0;
        end
      end
    end
  end

endmodule

`default_nettype wire
