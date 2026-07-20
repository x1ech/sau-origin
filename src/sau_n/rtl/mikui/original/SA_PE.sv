`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2024/12/12 14:33:00
// Design Name: 
// Module Name: SA_PE
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

`include "registers.svh"
module SA_PE
    import SA_pkg::*;
    #(
    parameter  type                   TagType      = logic,
    parameter  SA_pkg::int_format_e   IntFormat    = SA_pkg::INT8                  ,
    parameter  SA_pkg::int_format_e   IntFormat_q  = SA_pkg::INT16                 ,
    parameter  int unsigned           COL          = 1                             ,
    parameter  int unsigned           ROW          = 1                             ,
    parameter  int unsigned           OUTPUTDW     = 24                            ,
    parameter  int unsigned           CALC_DELAY   = 3                             ,
    parameter  int unsigned           DW           = SA_pkg::int_width(IntFormat)  ,
    parameter  int unsigned           QUANTDW      = 16
    )
    (
	  // interface to system
    input  logic                                   clk_i                      ,// CLK = 200MHz
    input  logic                                   rst_ni                     ,// rst_n, Negedge is active
    input  logic                                   EN_i                       ,// enable signal for the accelerator, high for active
    input  logic                                   pe_wstrb_i                 ,// mask or clear_reset
    output logic                                   pe_wstrb_o                 ,// mask or clear_reset
    // interface to pass data  .....
    input  logic signed [DW-1: 0]                  data_active_left_i         ,//data_in active_left
    output logic signed [DW-1: 0]                  data_active_right_o        ,//data_pass active_right
    input  logic signed [DW-1: 0]                  data_weight_above_i        ,// weighi flow in
    output logic signed [DW-1: 0]                  data_weight_below_o        ,// weighi flow in


    // interface to recomfigurable max find unit
    `ifdef MAX_USE
    input  logic signed [OUTPUTDW-1: 0]            local_max_i                ,// the max score in frow left
    output logic signed [OUTPUTDW-1: 0]            data_max_o                 ,//the  max score out 
    output logic                                   data_max_valid_o           ,
    `endif

    // interface to pass ins signal
    input  logic                                   acc_finish_flag_i          ,
    input  var SA_pkg::inst_t                      pe_inst_i                  ,//0: EN 1: C_EN 2-12:NUM 13:acc_valid
    output var SA_pkg::inst_t                      pe_inst_o                  ,//0: EN 1: C_EN 2-12:NUM 13:acc_valid 
    // interface to pass output result
    `ifdef  FULL_PRECISION
    input  logic signed [OUTPUTDW-1: 0]            data_bias_above_i          ,
    output logic signed [OUTPUTDW-1: 0]            data_out_sum_above_o       ,//final result
    `else
    input  logic signed [2*DW-1: 0]                data_bias_above_i          ,
    output logic signed [OUTPUTDW-1: 0]            data_out_sum_above_o       ,//final result
    `endif
    output logic                                   acc_valid_o                 // caculate finish signal 

    );
  localparam SIGN_EXTEND_BITS = OUTPUTDW - 2*(DW+1);
  logic                                   pe_wstrb;
  logic                                   mac_en_flag;
  logic                                   EN_o;// enable signal for the accelerator, high for active  
  logic                                   pe_conv_mode , pe_add_mode , pe_conv_or_add_mode;
  logic                                   pe_conv_mode_reg , pe_add_mode_reg;
  logic     signed    [DW-1: 0]           data_weight_above_t;//first row input data self delay
  logic     signed    [DW : 0]            dsp_in_a,
                                          dsp_in_b;
  logic     signed    [2*(DW+1)-1: 0]     dsp_result;
  logic               [OUTPUTDW-1:0]      sign_extension;
  logic     signed    [2*DW : 0]          data_multi_tmp;                 //dsp result 0 delay
  logic     signed    [OUTPUTDW-1: 0]     data_multi_tmp_reg;                 //dsp result 0 delay
  logic     signed    [OUTPUTDW-1: 0]     data_out_sum_tmp;              //dsp result 1 delay
  logic     signed    [DW-1: 0]           data_weight_below_r,
                                          data_active_right_r;                 //pass input data
  SA_pkg::inst_t                          pe_inst_reg;
  logic               [CALC_DELAY-2:0]    mac_en;
  logic               [CALC_DELAY-2:0]    mac_en_shift;
  logic                                   acc_finish_flag_d1 , acc_finish_flag_d2;
  logic                                   add_flag;
  logic                                   shift8_low_flag;
  logic                                   shift8_high_flag;
  logic     signed    [OUTPUTDW-1: 0]     data_out_sum_reg;              //caculate result

  assign mac_en_flag               = pe_wstrb & EN_i;
  assign pe_conv_mode              = (pe_inst_i.op_mode == SA_pkg::CONV) ? 1'b1 : 1'b0;
  assign pe_add_mode               = (pe_inst_i.op_mode == SA_pkg::ADD) ? 1'b1 : 1'b0;
  assign pe_conv_or_add_mode       = pe_conv_mode_reg | pe_add_mode_reg;
  assign shift8_low_flag           = pe_inst_o.shift_mode == 2'b10;//16bit mode low 8bit
  assign shift8_high_flag          = pe_inst_o.shift_mode == 2'b11;//16bit mode high 8bit
  assign dsp_in_a                  = shift8_low_flag ? {1'b0 , data_active_right_r} : {data_active_right_r[DW-1] , data_active_right_r};
  assign dsp_in_b                  = (pe_add_mode_reg & EN_o) ? {(DW+1){1'b1}} : {data_weight_below_r[DW-1] , data_weight_below_r};
  assign data_weight_below_o       = data_weight_below_r;
  assign data_active_right_o       = data_active_right_r;
  assign acc_valid_o               = (pe_conv_or_add_mode) ? acc_finish_flag_d2 : acc_finish_flag_d1;
  assign data_out_sum_above_o      = data_out_sum_reg;
  assign pe_inst_o                 = pe_inst_reg;
  assign add_flag                  = (acc_finish_flag_d1 & pe_conv_or_add_mode);
  generate
    if(CALC_DELAY <= 'd2)
      assign mac_en_shift = EN_o;
    else 
      assign mac_en_shift = {mac_en[CALC_DELAY-3:0] , EN_o};
  endgenerate
  // ------
  //------------------------Deliver sigals------------------------
  // ------
  `FFNR(pe_wstrb_o, pe_wstrb, clk_i)
  `FFNR(EN_o, mac_en_flag, clk_i)
  `FFNR(acc_finish_flag_d1, acc_finish_flag_i, clk_i)
  `FFNR(acc_finish_flag_d2, acc_finish_flag_d1, clk_i)
  `FFNR(pe_conv_mode_reg, pe_conv_mode, clk_i)
  `FFNR(pe_add_mode_reg, pe_add_mode, clk_i)
  `FFNR(pe_inst_reg, pe_inst_i, clk_i)
  `FFNR(mac_en, mac_en_shift, clk_i)
  `FFLNR(data_active_right_r, data_active_left_i, EN_i, clk_i)
  `FFLNR(data_weight_below_r, data_weight_above_t, EN_i, clk_i)
  //------------------------essential calcualtion------------------------
  // Multiplier
  // ------
  DW02_mult_2_stage #(.A_width((DW +1)),.B_width((DW +1)))u_dw_mult(
    .CLK                  (clk_i                     ),
    .TC                   (1'd1                      ),
    .A                    (dsp_in_a                  ),
    .B                    (dsp_in_b                  ),
    .PRODUCT              (dsp_result                )
    );
  assign data_multi_tmp = dsp_result[2*DW:0];
  // ------
  //Adder
  // ------
  logic               [OUTPUTDW-1: 0]augend,
                                     add_result_t;
  logic               add_state_valid;
  logic               [OUTPUTDW-1: 0]data_bias_above;
  logic               data_out_sum_tmp_clear_flag;
  logic               data_out_sum_tmp_clear_reg;
  assign sign_extension = (acc_finish_flag_i & pe_conv_or_add_mode) ? data_bias_above
                            : shift8_high_flag ? ( OUTPUTDW'(data_multi_tmp << 8)) :
                              {{SIGN_EXTEND_BITS{dsp_result[2*(DW+1)-1]}},dsp_result};
  `FFNR(data_multi_tmp_reg, sign_extension, clk_i)
  `ifdef  FULL_PRECISION
  assign data_bias_above          = data_bias_above_i;
  `else
  assign data_bias_above          = {{(OUTPUTDW-2*DW){data_bias_above_i[2*DW-1]}},data_bias_above_i};
  `endif
  assign augend                   = data_multi_tmp_reg;

  // Perform the addition using the selected inputs
  assign add_result_t             = SA_pkg::saturate_add_signed(data_out_sum_tmp , augend);
  assign add_state_valid          = add_flag | mac_en[CALC_DELAY-2];
  // ------
  //------------------------Output stage------------------------
  // ------
  assign data_out_sum_tmp_clear_flag = (acc_valid_o & ~pe_inst_reg.keep_mode) | (EN_o & pe_add_mode_reg);
  `FFNR(data_out_sum_tmp_clear_reg, data_out_sum_tmp_clear_flag, clk_i)
  `FFLARNC(data_out_sum_tmp, add_result_t, add_state_valid, data_out_sum_tmp_clear_reg,  '0, clk_i, rst_ni)
  `FFLNR(data_out_sum_reg, data_out_sum_tmp, acc_valid_o, clk_i)
  // ------
  //------------------------find max vlaue within row result------------------------
  // ------
  `ifdef MAX_USE
  logic               [OUTPUTDW-1: 0]     data_max_t;
  assign data_max_t               = (COL==0 || data_out_sum_tmp > local_max)
                                    ? data_out_sum_tmp : local_max_i;
  `FFL(data_max_o, data_max_t, acc_valid, '0, clk_i, rst_ni)
  `FF(data_max_valid_o, acc_valid, '0, clk_i, rst_ni)
  `endif
  // ------
  //------------------------Input delay------------------------
  // ------
generate
   if(ROW==0) begin : first_row_weight
    logic  weight_delay_en;
    assign weight_delay_en = ~pe_add_mode_reg;
    weight_delay#(.ROW(ROW),.COL(COL),.DATA_WIDTH(DW)) weight_delay_yes(
    .clk                                (clk_i                     ),
    .rst_n                              (rst_ni                    ),
    .EN                                 (weight_delay_en           ),
    .weight_i                           (data_weight_above_i       ),
    .weight_o                           (data_weight_above_t       )
      );
    // delay the write strobe signal
    weight_delay#(.ROW(ROW),.COL(COL),.DATA_WIDTH(1)) wstrb_delay_yes(
    .clk                                (clk_i                     ),
    .rst_n                              (rst_ni                    ),
    .EN                                 (weight_delay_en           ),
    .weight_i                           (pe_wstrb_i                ),
    .weight_o                           (pe_wstrb                  )
      );
   end
    else begin : nofirst_row_weight
    assign                data_weight_above_t       = data_weight_above_i;
    assign                pe_wstrb                  = pe_wstrb_i;
    end
endgenerate
endmodule
