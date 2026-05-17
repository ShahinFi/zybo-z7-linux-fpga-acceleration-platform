`timescale 1ns / 1ps
`default_nettype none

/* SPDX-License-Identifier: MIT */
/*
 * Fixed-XOR AXI-Stream validation accelerator.
 *
 * This block is a custom processing stage in the DMA stream path. It accepts
 * 32-bit AXI-Stream beats, XORs every valid data byte with 0xA5, and forwards
 * the stream without changing packet boundaries or transfer length.
 *
 * The accelerator uses a one-beat registered output stage. This keeps the
 * AXI-Stream handshake explicit and preserves output data, TKEEP, and TLAST
 * correctly whenever the downstream interface applies backpressure.
 */
module zybo_axis_xor_accel (
	input  wire        axis_aclk,
	input  wire        axis_aresetn,

	input  wire [31:0] s_axis_tdata,
	input  wire [3:0]  s_axis_tkeep,
	input  wire        s_axis_tvalid,
	input  wire        s_axis_tlast,
	output wire        s_axis_tready,

	output wire [31:0] m_axis_tdata,
	output wire [3:0]  m_axis_tkeep,
	output wire        m_axis_tvalid,
	output wire        m_axis_tlast,
	input  wire        m_axis_tready
);

	localparam [31:0] XOR_MASK = 32'hA5A5_A5A5;

	reg [31:0] data_q;
	reg [3:0]  keep_q;
	reg        last_q;
	reg        valid_q;

	/*
	 * The input side may accept a new beat when the output register is empty,
	 * or when the current output beat is being accepted in the same cycle.
	 */
	assign s_axis_tready = ~valid_q || m_axis_tready;

	assign m_axis_tdata  = data_q;
	assign m_axis_tkeep  = keep_q;
	assign m_axis_tlast  = last_q;
	assign m_axis_tvalid = valid_q;

	always @(posedge axis_aclk) begin
		if (!axis_aresetn) begin
			data_q  <= 32'd0;
			keep_q  <= 4'd0;
			last_q  <= 1'b0;
			valid_q <= 1'b0;
		end else if (s_axis_tready) begin
			valid_q <= s_axis_tvalid;

			if (s_axis_tvalid) begin
				data_q <= s_axis_tdata ^ XOR_MASK;
				keep_q <= s_axis_tkeep;
				last_q <= s_axis_tlast;
			end
		end
	end

endmodule

`default_nettype wire
