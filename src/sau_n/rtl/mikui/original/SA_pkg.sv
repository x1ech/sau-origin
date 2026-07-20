`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2024/12/12 14:39:07
// Design Name: 
// Module Name: SA_pkg
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////

package SA_pkg;

  localparam int unsigned NUM_INT_FORMATS = 4; // change me to add formats
  localparam int unsigned INT_FORMAT_BITS = $clog2(NUM_INT_FORMATS);
  localparam int unsigned OUTPUTDW = 24;
  localparam int unsigned SA_SIZE = 16;
  // Int formats
  typedef enum logic [INT_FORMAT_BITS-1:0] {
    INT8,
    INT16,
    INT32,
    INT64
    // add new formats here
  } int_format_e;

  // Returns the width of an INT format by index
  function automatic int unsigned int_width(int_format_e ifmt);
    unique case (ifmt)
      INT8:  return int'(8);
      INT16: return int'(16);
      INT32: return int'(32);
      INT64: return int'(64);
    endcase
  endfunction


  localparam int unsigned OP_BITS = 2;
  typedef enum logic [OP_BITS-1:0] {
    MATMUL = 2'b00,
    CONV = 2'b01,
    TRANSPOSER = 2'b10,
    ADD = 2'b11
  } calmode_e;

  // Returns the number of operands by operation group
  function automatic int unsigned num_operands(calmode_e grp);
    unique case (grp)
      MATMUL:  return int'(0);
      CONV:    return int'(1);
      TRANSPOSER: return int'(2);
      ADD:     return int'(3);
    endcase
  endfunction
  function automatic calmode_e mode_operands(logic [1:0] mode_i);
    case (mode_i)
      2'b00:  return MATMUL;
      2'b01:  return CONV;
      2'b10:  return TRANSPOSER;
      2'b11:  return ADD;
    endcase
  endfunction

  typedef enum logic [OP_BITS-1:0] {
    ABD = 2'b00,
    ATBD = 2'b01,
    ABTD = 2'b10,
    ABDT = 2'b11 
  } transmode_e;

  function automatic transmode_e mode_transpose(logic [OP_BITS-1:0] mode_i);
    case (mode_i)
      2'b00:  return ABD;
      2'b01:  return ATBD;
      2'b10:  return ABTD;
      2'b11:  return ABDT;
    endcase
  endfunction
  typedef enum logic [OP_BITS-1:0] {
    CNORMAL = 2'b00,
    CTRANS  = 2'b01,
    RETAIN  = 2'b10,
    TRETAIN = 2'b11
  } flowmode_e;

  function automatic flowmode_e mode_outflow(logic [OP_BITS-1:0] mode_i);
    case (mode_i)
      2'b00:  return CNORMAL;
      2'b01:  return CTRANS;
      2'b10:  return RETAIN;
      2'b11:  return TRETAIN;
    endcase
  endfunction
//todo: add sa_mode flow mode eg..

  typedef enum logic [2:0] {
  IDLE          = 3'b000,
  FIRST_LOAD    = 3'b001,
  REGISTER_LOAD = 3'b010,
  REUSE_LOAD    = 3'b011,
  D_OUT         = 3'b110
  } registerfile_state_e;

  typedef struct packed {
    logic [   1: 0] shift_mode;
    logic [   0: 0] keep_mode;
    logic [   1: 0] op_mode;
  } inst_t;

// -----------------------------------------------------------------------------
// Signed Saturation Adder Function
// Assigns 'add_result_t = data_out_sum_tmp + augend;' with signed saturation
// -----------------------------------------------------------------------------

  function automatic signed [OUTPUTDW-1:0] saturate_add_signed(
      input  signed [OUTPUTDW-1:0] SUM_A,
      input  signed [OUTPUTDW-1:0] SUM_B
  );
      // Define the maximum and minimum values for the given WIDTH
      // For signed N-bit numbers (2's complement):
      // Max = 2^(N-1) - 1  (e.g., for 8-bit, 0111_1111 = 127)
      // Min = -2^(N-1)     (e.g., for 8-bit, 1000_0000 = -128)
      localparam signed [OUTPUTDW-1:0] MAX_SIGNED_VAL = {1'b0, {OUTPUTDW-1{1'b1}}};
      localparam signed [OUTPUTDW-1:0] MIN_SIGNED_VAL = {1'b1, {OUTPUTDW-1{1'b0}}};

      // Perform standard WIDTH-bit signed addition
      logic signed [OUTPUTDW-1:0] sum_normal;
      logic positive_overflow;
      logic negative_underflow;
      sum_normal = SUM_A + SUM_B;

      // Detect positive overflow: (A > 0 AND B > 0 AND Sum < 0)
      // This happens if the sign bit of sum_normal is 1 (negative) while inputs were 0 (positive)
      positive_overflow = (SUM_A[OUTPUTDW-1] == 1'b0) && // data_out_sum_tmp is positive
                          (SUM_B[OUTPUTDW-1] == 1'b0) && // augend is positive
                          (sum_normal[OUTPUTDW-1] == 1'b1);     // sum_normal became negative

      // Detect negative underflow: (A < 0 AND B < 0 AND Sum > 0)
      // This happens if the sign bit of sum_normal is 0 (positive) while inputs were 1 (negative)
      negative_underflow = (SUM_A[OUTPUTDW-1] == 1'b1) && // data_out_sum_tmp is negative
                          (SUM_B[OUTPUTDW-1] == 1'b1) && // augend is negative
                          (sum_normal[OUTPUTDW-1] == 1'b0);     // sum_normal became positive
      if (positive_overflow) begin
          saturate_add_signed = MAX_SIGNED_VAL; // Clamp to max positive value
      end else if (negative_underflow) begin
          saturate_add_signed = MIN_SIGNED_VAL; // Clamp to max negative value (min value)
      end else begin
          saturate_add_signed = sum_normal;     // No overflow/underflow, use normal sum
      end
  endfunction

  function automatic logic [5:0] get_exe_cycle(
    input logic [2:0] conv_kernal_i // 输入是3位宽 (3-1:0)
  );
    // 声明一个局部变量来存储结果，然后赋值给函数名
    logic [5:0] exe_cycle_val; // 输出是6位宽 (6-1:0)
    case (conv_kernal_i)
        3'd3:  exe_cycle_val = 6'd9;  // 当 conv_kernal_i 为 3 时，EXE_CYCLE 为 9
        3'd5:  exe_cycle_val = 6'd25; // 当 conv_kernal_i 为 5 时，EXE_CYCLE 为 25
        3'd7:  exe_cycle_val = 6'd49; // 当 conv_kernal_i 为 7 时，EXE_CYCLE 为 49
        default: exe_cycle_val = 6'(SA_SIZE);  // 其他所有情况 (0, 1, 2, 4, 6)，EXE_CYCLE 为 8
    endcase

    return exe_cycle_val; // 返回计算出的 EXE_CYCLE 值
endfunction

  function automatic void split_even_odd(
    input  logic [2*SA_SIZE*8-1:0]  in_data,         
    output logic [  SA_SIZE*8-1:0]  out_even_chunks,
    output logic [  SA_SIZE*8-1:0]  out_odd_chunks
    );
    for (int i = 0; i < SA_SIZE; i = i + 1) begin
        out_even_chunks[i*8 +: 8] = in_data[(2*i)*8 +: 8];
        out_odd_chunks[i*8 +: 8] = in_data[(2*i + 1)*8 +: 8];
    end
  endfunction

  function automatic logic [127:0] extend_64_to_128_optimal(
    input logic [63:0] in_64
  );
    return {
        {8{in_64[63]}}, in_64[63:56],
        {8{in_64[55]}}, in_64[55:48],
        {8{in_64[47]}}, in_64[47:40],
        {8{in_64[39]}}, in_64[39:32],
        {8{in_64[31]}}, in_64[31:24],
        {8{in_64[23]}}, in_64[23:16],
        {8{in_64[15]}}, in_64[15:8],
        {8{in_64[7]}},  in_64[7:0]
    };
  endfunction


  function automatic logic signed [15:0] sat_truncate_func (
      input logic signed [23:0] in_data,
      input logic [4:0]         cutbit,
      input bit                 shift
  );
      logic signed [23:0] shifted;
      logic signed [15:0] trunc_val;
      logic sign_bit, overflow;
      int i;

      begin
          // 把 in_data 右移 cutbit 位，然后取低 16 位
          shifted   = in_data >>> cutbit;
          trunc_val = shifted[15:0];
          sign_bit  = in_data[23];

          // 检查高位是否全等于 sign_bit
          if (shift)
              overflow = |(shifted[23:15] ^ {9{sign_bit}});
          else // 8-bit mode
              overflow = |(shifted[23:7] ^ {17{sign_bit}});
          // 饱和处理
          if (overflow) begin
              if (sign_bit)
                  sat_truncate_func = shift ? 16'sh8000 : 16'shFF80;
              else
                  sat_truncate_func = shift ? 16'sh7FFF : 16'sh007F;
          end else begin
              sat_truncate_func = trunc_val;
          end
      end
  endfunction


endpackage