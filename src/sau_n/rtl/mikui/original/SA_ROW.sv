`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2024/12/12 14:34:18
// Design Name: 
// Module Name: SA_ROW
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
// `define MAX_USE
// `define FULL_PRECISION
`include "registers.svh"
module SA_ROW
    import SA_pkg::*;
    #(
    parameter  type                   TagType      = logic,
    parameter  SA_pkg::int_format_e   IntFormat    = SA_pkg::INT8                  ,
    parameter  SA_pkg::int_format_e   IntFormat_q  = SA_pkg::INT16                 ,
    parameter  int unsigned           COL_NUM      = 1                             ,
    parameter  int unsigned           ROW_SEQ      = 1                             ,
    parameter  int unsigned           OUTPUTDW     = 24                            ,
    parameter  int unsigned           CNT_DW       = 10                            ,
    parameter  int unsigned           CALC_DELAY   = 3                             ,
    parameter int unsigned            DW           = 8                             ,
    parameter int unsigned            QUANTDW      = 16
    )
    (
    input  logic        clk_i,                                  // CLK = 200MHz
    input  logic        rst_ni,                                 // RESET, Negedge is active
    input  logic        EN_i,                                   // enable data move (each row same)
    input  logic        C_EN_i,                                 // caculate enable  (each row differrent)
    input  logic        stop_en_i,                              // input data valid
    input  logic        shift_ctl_i,
    input  logic        shift_mode_i,
    input  logic        [   1: 0]sa_mode_i,                     // pe caculate way
    input  logic        [   1: 0]sa_flowmode_i,                 // output data flowmode
    input  logic        [CNT_DW: 0]CALC_CYCLE_i,                // calculate cycle
    input  logic        [$clog2(COL_NUM): 0]FINISH_COL_i,       // Effective PE
    input  logic        [COL_NUM-1: 0]pe_wstrb_i,               // PE write strobe signal
    output logic        [COL_NUM-1: 0]pe_wstrb_o,               // PE write strobe signal

// interface to PE array .....
    //move data from PE array
    input  logic signed [DW-1: 0]active_left_i,                 // left data
    input  logic        [COL_NUM*DW-1: 0]in_weight_above_i,     // up data
    output logic        [COL_NUM*DW-1: 0]out_weight_below_o,    // down data
    //finial output data
    `ifdef FULL_PRECISION
    input  logic        [COL_NUM*OUTPUTDW-1: 0]in_bias_above_i, // up data
    output logic        [COL_NUM*OUTPUTDW-1: 0]score_row_o,    // current row result
    `else
    input  logic        [COL_NUM*2*DW-1: 0]in_bias_above_i, // up data
    `endif
    output logic        [COL_NUM*OUTPUTDW-1: 0]score_row_o_q,   // current row result_q

`ifdef MAX_USE
    output logic        [OUTPUTDW-1: 0]data_max_row_o,
    output logic           data_max_valid_o,
`endif
    output logic           C_EN_interrupt_o,
    output logic           PE_valid_o,
    output logic           OS_valid_o                           //row finish signal
    );
  //interface to wire each pe
  `ifdef FULL_PRECISION
  logic               [COL_NUM*OUTPUTDW-1: 0]   out_sum_row;     // output mac row data
  `else
  logic               [COL_NUM*OUTPUTDW-1: 0]   out_sum_row;     // output mac row data
  `endif
  logic               [COL_NUM*DW-1: 0]         active_right;    // left data move to right
  logic               [COL_NUM*OUTPUTDW-1: 0]   out_sum_row_q;   // output mac row data quantizatio
  logic               [COL_NUM-1: 0]            acc_finish_flag_i;
  logic               [COL_NUM*2*DW-1: 0]       in_bias_above_case;
  logic               [COL_NUM*2*DW-1: 0]       in_bias_above;
  logic               [COL_NUM-1: 0]            pe_en_i;           //each pe input data valid
  logic               [COL_NUM-1: 0]            acc_valid;       // each pe state in row
  //system sognal
  logic     signed    [DW-1: 0]                 active_left_d,   // left data delay
                                                active_left_in;  // left data to systolic array
  logic                                         CALC_CYCLE_cnt_flag;// pe start caculating flag
  logic                                         matrix_add_mode;// matrix add mode
  logic                                         CALC_CYCLE_done_flag;
  logic                                         acc_valid_last_flag;
  logic                                         acc_valid_d;     // acc_valid delay
  logic                                         C_EN_valid,
                                                C_EN_delay;
  logic               [CNT_DW: 0]               CALC_CYCLE_cnt;// each row have one cnt to judge the end of caculating
  logic                                         acc_valid_begin;// output signal
  logic                                         stop_en;
  logic               [$clog2(COL_NUM): 0]      acc_valid_cnt;
  logic                                         acc_valid_0;
  logic               [CALC_DELAY-2: 0]         acc_valid_0_d;
  logic                                         bias_update_case;
  `ifdef MAX_USE
  logic  signed       [COL_NUM*OUTPUTDW-1: 0]   data_max  ;//data_max  each  pe
  logic               [COL_NUM-1: 0]            data_max_valid_row  ;//data_max_valid  each  pe
  assign data_max_valid_o = data_max_valid_row[COL_NUM-1];
  assign data_max_row_o = data_max[(FINISH_COL_i*OUTPUTDW-1)-:OUTPUTDW];
  `endif
  assign CALC_CYCLE_cnt_flag = !stop_en && stop_en_i;//1 delay
  assign active_left_in = active_left_d;
  assign OS_valid_o  = acc_valid_d;
  `ifdef FULL_PRECISION
  assign score_row_o = out_sum_row;
  `endif
  assign score_row_o_q = out_sum_row_q;
  assign CALC_CYCLE_done_flag = CALC_CYCLE_cnt==CALC_CYCLE_i;
  assign acc_valid_last_flag = acc_valid_cnt == FINISH_COL_i-1;
  assign matrix_add_mode = sa_mode_i == 2'b11;
  assign in_bias_above_case = matrix_add_mode ? {8'd0,in_weight_above_i} : in_bias_above_i;
  assign bias_update_case = (ROW_SEQ == 0) ? stop_en_i : stop_en;
  `FFLNR(in_bias_above, in_bias_above_case, bias_update_case, clk_i)
  `FF(acc_valid_d, acc_valid[FINISH_COL_i-1], '0 , clk_i, rst_ni)
  `FF(PE_valid_o, acc_valid[0], '0 , clk_i, rst_ni)
  // ------
  //------------------------Input delay------------------------
  // ------
  SA_pkg::inst_t pe_inst_t;
  SA_pkg::inst_t pe_inst_0;
  SA_pkg::inst_t pe_inst[COL_NUM];
  logic  [CALC_DELAY-2: 0]acc_valid_0_d_shift;
  logic  [CALC_DELAY-2: 0]acc_valid_flag;
  logic  [16-1: 0]stop_en_t , acc_finish_flag_t;
  logic  [16-1: 0]stop_en_t_reg , acc_finish_flag_t_reg;
  assign pe_inst_t = '{shift_mode: {shift_mode_i,shift_ctl_i}, keep_mode: sa_flowmode_i[1], op_mode: sa_mode_i};
  assign acc_valid_flag = acc_valid_0_d_shift;
  assign stop_en_t = matrix_add_mode ? {16{stop_en_i}} : {stop_en_t_reg[16-2:0],stop_en_i};
  assign acc_finish_flag_t = matrix_add_mode ? {16{acc_valid_0_d[CALC_DELAY-3]}} : {acc_finish_flag_t_reg[16-2:0],acc_valid_0_d[CALC_DELAY-3]};
  assign pe_en_i = stop_en_t_reg;
  assign acc_finish_flag_i = acc_finish_flag_t_reg;
  `FF(stop_en_t_reg, stop_en_t, '0, clk_i, rst_ni)
  `FF(acc_finish_flag_t_reg, acc_finish_flag_t, '0, clk_i, rst_ni)
  `FF(C_EN_delay, C_EN_i, '0, clk_i, rst_ni)
  `FF(stop_en, stop_en_i, '0, clk_i, rst_ni)
  `FF(pe_inst_0, pe_inst_t, '0, clk_i, rst_ni)
  `FF(acc_valid_0_d, acc_valid_flag, '0, clk_i, rst_ni)
  generate if(CALC_DELAY == 2)begin
    assign acc_valid_0_d_shift = acc_valid_0;
  end
  else begin
    assign acc_valid_0_d_shift = {acc_valid_0_d[CALC_DELAY-3:0],acc_valid_0};
  end
  endgenerate
  // ------
  //------------------------Generate of every PE------------------------
  // ------
genvar gi;
generate
        for(gi = 0; gi < COL_NUM; gi = gi + 1)                      //16 PE
        begin:PE_COL
        localparam int COL_SEQ_ID = gi;
            // some reg/wire variables for each PE
            // .......
        if(gi == 0)begin
            SA_PE #(
            .COL                    (COL_SEQ_ID                ),
            .ROW                    (ROW_SEQ                   ),
            .IntFormat              (IntFormat                 ),
            .IntFormat_q            (IntFormat_q               ),
            .OUTPUTDW               (OUTPUTDW                  ),
            .CALC_DELAY             (CALC_DELAY                )
            ) 
            PE_unit(
            .clk_i                  (clk_i                     ),
            .rst_ni                 (rst_ni                    ),
            .EN_i                   (pe_en_i[gi]               ),
            .pe_wstrb_i             (pe_wstrb_i[gi]            ),
            .pe_wstrb_o             (pe_wstrb_o[gi]            ),
                        // .....
            `ifdef MAX_USE
            .local_max              ({24{1'b1}}                ),
            .data_max               (data_max[OUTPUTDW-1:0]    ),
            .data_max_valid         (data_max_valid_row[gi]    ),
            `endif

            .data_active_left_i     (active_left_in            ),
            .data_active_right_o    (active_right[DW-1:0] ),
            .data_weight_above_i    (in_weight_above_i[DW-1:0]),
            .data_weight_below_o    (out_weight_below_o[DW-1:0]),
            .acc_finish_flag_i      (acc_finish_flag_i[gi]     ),
            .pe_inst_i              (pe_inst_0                 ),
            .pe_inst_o              (pe_inst[gi]               ),
            `ifdef FULL_PRECISION
            .data_bias_above_i      (in_bias_above[OUTPUTDW-1:0]),
            .data_out_sum_above_o   (out_sum_row[OUTPUTDW-1:0] ),
            `else
            .data_bias_above_i      (in_bias_above[2*DW-1:0]),
            .data_out_sum_above_o   (out_sum_row[OUTPUTDW-1:0]  ),
            `endif
            .acc_valid_o            (acc_valid[0]              )
            );
        end
        else  begin
            SA_PE #(
            .COL                    (COL_SEQ_ID                ),
            .ROW                    (ROW_SEQ                   ),
            .IntFormat              (IntFormat                 ),
            .IntFormat_q            (IntFormat_q               ),
            .OUTPUTDW               (OUTPUTDW                  ),
            .CALC_DELAY             (CALC_DELAY                )
            )
            PE_unit(
            .clk_i                  (clk_i                     ),
            .rst_ni                 (rst_ni                    ),
            .EN_i                   (pe_en_i[gi]               ),
            .pe_wstrb_i             (pe_wstrb_i[gi]            ),
            .pe_wstrb_o             (pe_wstrb_o[gi]            ),
                        // .....
            `ifdef MAX_USE
            .local_max              (data_max[gi*OUTPUTDW-1:(gi-1)*OUTPUTDW]),
            .data_max               (data_max[(gi+1)*OUTPUTDW-1:gi*OUTPUTDW]),
            .data_max_valid         (data_max_valid_row[gi]                 ),
            `endif
            .data_active_left_i     (active_right[gi*DW-1:(gi-1)*DW]        ),
            .data_active_right_o    (active_right[(gi+1)*DW-1:gi*DW]        ),
            .acc_finish_flag_i      (acc_finish_flag_i[gi]                  ),
            .pe_inst_i              (pe_inst[gi-1]                          ),
            .pe_inst_o              (pe_inst[gi]                            ),
            .data_weight_above_i    (in_weight_above_i[(gi+1)*DW-1:gi*DW]   ),
            .data_weight_below_o    (out_weight_below_o[(gi+1)*DW-1:gi*DW]  ),

            `ifdef FULL_PRECISION
            .data_bias_above_i      (in_bias_above[(gi+1)*OUTPUTDW-1:gi*OUTPUTDW]),
            .data_out_sum_above_o   (out_sum_row[(gi+1)*OUTPUTDW-1:gi*OUTPUTDW]),
            `else
            .data_bias_above_i      (in_bias_above[(gi+1)*2*DW-1:gi*2*DW]),
            .data_out_sum_above_o   (out_sum_row[(gi+1)*OUTPUTDW-1:gi*OUTPUTDW]),
            `endif
            .acc_valid_o            (acc_valid[gi]             )
                        );
        end
    assign out_sum_row_q[(gi+1)*OUTPUTDW-1:gi*OUTPUTDW]= out_sum_row[(gi+1)*OUTPUTDW-1:gi*OUTPUTDW];
    end
endgenerate
  // ------
  //------------------------Ctrl signal------------------------
  // ------
  logic pe_execute_stop;
  logic [$clog2(COL_NUM): 0]acc_valid_cnt_result;
  logic C_EN_interrupt_flag;
  logic C_EN_valid_flag,C_EN_valid_n;
  logic [CNT_DW: 0] CALC_CYCLE_cnt_result;
  logic CALC_CYCLE_cnt_clear;
  logic active_delay_en;
  assign pe_execute_stop = CALC_CYCLE_cnt==CALC_CYCLE_i-1 && stop_en;
  assign acc_valid_cnt_result = acc_valid_begin?acc_valid_cnt + 1 : 'd0;
  assign C_EN_interrupt_flag = acc_valid_begin && stop_en_i;
  assign C_EN_valid_flag = !C_EN_interrupt_o  && acc_valid_last_flag;
  assign CALC_CYCLE_cnt_result = CALC_CYCLE_cnt + 1;
  assign CALC_CYCLE_cnt_clear =  C_EN_valid && CALC_CYCLE_done_flag || !C_EN_delay;
  assign C_EN_valid_n = !C_EN_valid;
  assign active_delay_en = EN_i && (sa_mode_i != 2'b11);
  `FF(acc_valid_0, pe_execute_stop, '0, clk_i, rst_ni)
  `FFARNC(acc_valid_cnt, acc_valid_cnt_result, acc_valid_last_flag, '0, clk_i, rst_ni)
  `FFLARNC(acc_valid_begin, 1'd1, pe_execute_stop, acc_valid_last_flag, '0, clk_i, rst_ni)
  `FFARNC(C_EN_interrupt_o, C_EN_interrupt_flag, C_EN_valid_n, '0, clk_i, rst_ni)
  `FFLARNC(C_EN_valid, 1'd1, CALC_CYCLE_cnt_flag, C_EN_valid_flag, '0, clk_i, rst_ni)
  `FFLARNC(CALC_CYCLE_cnt, CALC_CYCLE_cnt_result, stop_en, CALC_CYCLE_cnt_clear, '0, clk_i, rst_ni)

  active_delay#(
    .ROW                  (ROW_SEQ                   ),
    .INPUTDW              (DW                        )
    )
    u_active_delay(
    .clk                  (clk_i                      ),
    .rst_n                (rst_ni                     ),
    .active_i             (active_left_i              ),
    .EN                   (active_delay_en            ),
    .active_o             (active_left_d              )
    );
endmodule
