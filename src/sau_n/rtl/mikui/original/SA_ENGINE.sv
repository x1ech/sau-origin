`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2024/12/12 14:34:18
// Design Name: 
// Module Name: SA_ENGINE
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
module SA_ENGINE
    import SA_pkg::*;
    #(
    parameter  type                   TagType      = logic,
    parameter  SA_pkg::int_format_e   IntFormat    = SA_pkg::INT8                  ,
    parameter  SA_pkg::int_format_e   IntFormat_q  = SA_pkg::INT16                 ,
    parameter  int unsigned           ROW_NUM      = 16                             ,
    parameter  int unsigned           COL_NUM      = 16                             ,
    parameter  int unsigned           OUTPUTDW     = 24                            ,
    parameter  int unsigned           CNT_DW       = 10                            ,
    parameter  int unsigned           INPUTDW      = 8  ,
    parameter  int unsigned           QUANTDW      = 16
    )
    (
    input        logic                         clk,// CLK = 200MHz
    input        logic                         rst_n,// RESET, Negedge is active
    input        logic                         EN_i,// enable signal for the accelerator, high for active
    input        logic                         Flag_o,// enable signal for the accelerator, high for active
    input        logic                         Flag_o_ready,
    input        logic                         ins_valid_i,// enable output for the accelerator, high for active
    input        logic        [   1: 0]        sa_calmode_i,//00 : gemm  01:transposer 10:conv  11:matrix add 
    input        logic        [   1: 0]        sa_flowmode_i,//00 : clear  01:Output line by line  10:Output column by column  
    input        logic        [   1: 0]        register_mode_i,
    input        logic                         shift_mode_i,
    input        logic        [CNT_DW: 0]      CALC_CYCLE_i,
    input        logic        [$clog2(ROW_NUM): 0]row_num_i,
    input        logic        [$clog2(COL_NUM): 0]col_num_i,
    input        logic        [$clog2(OUTPUTDW)-1: 0]cutbit,//0628test
    input        logic                         shift_ctl_i,
    input        logic        [ROW_NUM*INPUTDW-1: 0]data_active_left,
    input        logic        [COL_NUM*INPUTDW-1: 0]in_weight_above,
    `ifdef FULL_PRECISION
    input        logic        [COL_NUM*OUTPUTDW-1: 0]in_bias_above,
    output       logic        [COL_NUM*OUTPUTDW-1: 0]out_sum_final,
    `else
    input        logic        [COL_NUM*2*INPUTDW-1: 0]in_bias_above,
    `endif
    output       logic        [COL_NUM*QUANTDW-1: 0] out_sum_final_q,
    output       logic                          row_score_valid,
    output       logic        [$clog2(ROW_NUM): 0]row_seq_o,
    `ifdef MAX_USE
    output       logic        [OUTPUTDW-1: 0]  data_max_row_o,
    `endif
    output       logic                         storage_ready,
    output       logic                         pe_finish_o,
    output       logic                         cal_finish
    );
    typedef enum logic [2:0] {
        IDLE    = 3'b000,
        START   = 3'b001,
        WORK    = 3'b010,
        STORAGE = 3'b011,
        DONE    = 3'b100
    } SA_current_state_t;
    SA_current_state_t sa_cur_state, sa_next_state;
    `ifdef FULL_PRECISION
    logic       [ROW_NUM*OUTPUTDW*COL_NUM-1: 0]out_sum;
    `endif
    `ifdef MAX_USE
    logic       [ROW_NUM*OUTPUTDW-1: 0]data_max_sa;
    `endif
    logic       clear_mode;
    logic       [$clog2(ROW_NUM): 0]FINISH_ROW;
    logic       [$clog2(COL_NUM): 0]FINISH_COL;
    logic       row_en[0:ROW_NUM-1];
    logic       c_en[0:ROW_NUM-1];
    logic       [ROW_NUM*INPUTDW*COL_NUM-1: 0]out_weight_below;
    logic       [ROW_NUM*OUTPUTDW*COL_NUM-1: 0]out_sum_q;
    logic       [COL_NUM-1: 0]pe_wstrb[ROW_NUM];
    logic       [COL_NUM*INPUTDW-1:0]in_weight_above_t[0:ROW_NUM-1];
    logic       [COL_NUM-1:0]pe_wstrb_t[ROW_NUM];
    logic       [ROW_NUM-1: 0]OS_valid;
    logic       [ROW_NUM-1: 0]PE_valid;
    logic       EN;
    logic       [ROW_NUM-1: 0]stop_en_i;
    logic       [ROW_NUM-1: 0]en_comflict;

    logic       [CNT_DW: 0]CALC_CYCLE;
    logic       [ROW_NUM-1: 0]stop_en;
    logic       SA_en;
    logic       [$clog2(ROW_NUM): 0]row_cnt;
    logic       [CNT_DW: 0]datain_cnt;
    logic       [   1: 0]sa_mode;
    logic       [   1: 0]sa_flowmode;
    logic       [   1: 0]register_mode;
    logic       [ROW_NUM-1: 0]shift_ctl;
    logic       shift_mode;
    logic       EN_n;
    logic       matrix_add_mode;
    logic       single_col_mode;

    assign      FINISH_ROW = (($clog2(COL_NUM))'(ROW_NUM)>=row_num_i)?row_num_i:ROW_NUM;
    assign      FINISH_COL = (($clog2(COL_NUM))'(COL_NUM)>=col_num_i)?col_num_i:COL_NUM;
    assign      EN = SA_en| EN_i;
    assign      clear_mode = OS_valid[0] && ~sa_flowmode[1];
    assign      EN_n = !EN;
    assign      matrix_add_mode = (sa_mode == 2'b11) ? 1'b1 : 1'b0;
    assign      single_col_mode = (register_mode == 2'b10) ? 1'b1 : 1'b0;
//------------------------SA STATEMENT------------------------
  `FF(sa_cur_state, sa_next_state, IDLE, clk, rst_n)
  always_comb begin: sa_state
    sa_next_state = sa_cur_state;
    case(sa_cur_state)
        IDLE:
            begin
                if(EN_i)
                    sa_next_state = START;
                else
                    sa_next_state = IDLE;
            end
        START:
            begin
                if(EN_i && datain_cnt == (CALC_CYCLE-1) )
                    sa_next_state = WORK;
                else if(cal_finish)
                    sa_next_state = STORAGE;
                else if (!EN_i && datain_cnt == 'd0  )
                    sa_next_state = IDLE;
                else
                    sa_next_state = START;
            end
        WORK:
            begin
                if(OS_valid[0] && !EN_i)
                    sa_next_state = STORAGE;
                else
                    sa_next_state = WORK;
            end
        STORAGE:
            begin
                if(cal_finish)
                    sa_next_state = IDLE;
                else if(EN_i)
                    sa_next_state = START;
                else
                    sa_next_state = STORAGE;
            end
        DONE:
            begin
                if(Flag_o)
                    sa_next_state = IDLE;
                else
                    sa_next_state = DONE;
            end
        default: sa_next_state= IDLE;
    endcase
  end
//------------------------SA INSTRUCTION------------------------
  logic ins_update_flag;
  assign ins_update_flag = (ins_valid_i & sa_cur_state != WORK) | OS_valid[0];
  `FFL(sa_mode, sa_calmode_i, ins_update_flag, '0, clk, rst_n)
  `FFL(sa_flowmode, sa_flowmode_i, ins_update_flag, '0, clk, rst_n)
  `FFL(CALC_CYCLE, CALC_CYCLE_i, ins_update_flag, '0, clk, rst_n)
  `FFL(register_mode, register_mode_i, ins_update_flag, '0, clk, rst_n)
  `FFL(shift_mode, shift_mode_i, ins_update_flag, '0, clk, rst_n)
//------------------------INPUT SA STATE CNT/VALID------------------------
  logic  row_cnt_valid;
  logic  [$clog2(ROW_NUM): 0]row_cnt_result;
  logic  [CNT_DW: 0]datain_cnt_result;
  logic  datain_cnt_valid;
  logic  [ROW_NUM-1: 0]stop_en_t;
  logic  storage_ready_flag;
  logic  storage_ready_flag_clear;
  logic  [ROW_NUM-1: 0]shift_ctl_reg;
  logic  [ROW_NUM-1: 0]shift_ctl_t;
  assign row_cnt_valid = (row_cnt < (FINISH_ROW-1));
  assign row_cnt_result = row_cnt + 1;
  assign datain_cnt_result = datain_cnt + 1;
  assign datain_cnt_valid = (EN_i && datain_cnt < (CALC_CYCLE-1));
  assign stop_en_t = EN ? {stop_en[ROW_NUM-2:0],EN_i} : 'd0;
  assign storage_ready_flag = OS_valid[0] && ~sa_flowmode[1];//
  assign pe_finish_o = PE_valid[0];
  assign storage_ready_flag_clear = cal_finish;
  assign shift_ctl_t = {shift_ctl_reg[ROW_NUM-2:0],shift_ctl_i};
  `FFLARNC(row_cnt, row_cnt_result, row_cnt_valid, EN_n, '0, clk, rst_n)
  `FFLARNC(datain_cnt, datain_cnt_result, datain_cnt_valid, EN_n, '0, clk, rst_n)
  `FFLARNC(SA_en, 1'd1, EN_i, clear_mode, '0, clk, rst_n)
  `FF(stop_en, stop_en_t, '0, clk, rst_n)
  `FFLARNC(storage_ready, 1'd1, storage_ready_flag, storage_ready_flag_clear, '0, clk, rst_n)
  `FF(shift_ctl_reg, shift_ctl_t, '0, clk, rst_n)
//------------------------SA WORK ENABLE------------------------
  logic  [COL_NUM-1 : 0]pe_wstrb_0;
  typedef struct packed {
    logic en;
    logic [COL_NUM-1 : 0]col_wstrb;
    logic [COL_NUM-1 : 0]col_wstrb_reg;
    logic update_flag;
    logic clear_flag;
  } single_col_t;
  single_col_t DWCONV_mode;
  assign DWCONV_mode.en = single_col_mode;
  assign DWCONV_mode.update_flag = stop_en[0] && !EN_i && (shift_mode == shift_ctl[0]);
  assign DWCONV_mode.clear_flag = storage_ready_flag;
  `ifdef MODULE_TEST
  localparam [ROW_NUM-1:0]CLEAR_VALUE = {1'b1, {(ROW_NUM-1){1'b0}}};
  assign DWCONV_mode.col_wstrb = {DWCONV_mode.col_wstrb_reg[0], DWCONV_mode.col_wstrb_reg[COL_NUM-1:1]};
  `FFLARNC(DWCONV_mode.col_wstrb_reg, DWCONV_mode.col_wstrb, DWCONV_mode.update_flag, DWCONV_mode.clear_flag, CLEAR_VALUE, clk, rst_n)
  `else
  localparam [COL_NUM-1:0]CLEAR_VALUE = {{(COL_NUM-1){1'b0}}, 1'b1};
  assign DWCONV_mode.col_wstrb = {DWCONV_mode.col_wstrb_reg[COL_NUM-2:0], DWCONV_mode.col_wstrb_reg[COL_NUM-1]};
  `FFLARNC(DWCONV_mode.col_wstrb_reg, DWCONV_mode.col_wstrb, DWCONV_mode.update_flag, DWCONV_mode.clear_flag, CLEAR_VALUE, clk, rst_n)
  `endif
  assign pe_wstrb_0 = single_col_mode ? DWCONV_mode.col_wstrb_reg : {COL_NUM{1'b1}};
//------------------------generate of every PE row------------------------
genvar gi,a;
generate
    for(gi = 0; gi < ROW_NUM; gi = gi + 1)                          //16 row
    begin:PE_row
    localparam int ROW_SEQ_ID = gi;
    // some reg/wire variables for each row
    // .......
    assign                              row_en[gi]                = ((gi<=row_cnt))?1'b1:1'b0;
    assign                              c_en[gi]                  = EN & (matrix_add_mode ? 1'd1 : row_en[gi]);
    if(gi==0)begin
        assign in_weight_above_t[gi] = in_weight_above;
        assign stop_en_i[gi] = EN_i;
        assign pe_wstrb_t[gi] = pe_wstrb_0;
        assign shift_ctl[gi] = shift_ctl_i;
    end
    else begin
        assign in_weight_above_t[gi] = out_weight_below[COL_NUM*INPUTDW*gi-1:COL_NUM*INPUTDW*(gi-1)];
        assign stop_en_i[gi] = matrix_add_mode ? EN_i : stop_en[gi-1];
        assign pe_wstrb_t[gi] = pe_wstrb[gi-1];
        assign shift_ctl[gi] = shift_ctl_reg[gi-1];
    end
    SA_ROW #(
    .COL_NUM                              (COL_NUM                    ),
    .CNT_DW                               (CNT_DW                     ),
    .IntFormat                            (IntFormat                  ),
    .IntFormat_q                          (IntFormat_q                ),
    .OUTPUTDW                             (OUTPUTDW                   ),
    .ROW_SEQ                              (ROW_SEQ_ID                 )
    )
    PE_row_unit(
    .clk_i                                (clk                       ),
    .rst_ni                               (rst_n                     ),
    .EN_i                                 (EN                        ),
    .C_EN_i                               (c_en[gi]                  ),//row en
    .stop_en_i                            (stop_en_i[gi]             ),
    .shift_ctl_i                          (shift_ctl[gi]             ),
    .C_EN_interrupt_o                     (en_comflict[gi]           ),
    .sa_mode_i                            (sa_mode                   ),
    .sa_flowmode_i                        (sa_flowmode               ),
    .shift_mode_i                         (shift_mode                ),
    .FINISH_COL_i                         (FINISH_COL                ),
    .pe_wstrb_i                           (pe_wstrb_t[gi]            ),
    .pe_wstrb_o                           (pe_wstrb[gi]              ),
    .CALC_CYCLE_i                         (CALC_CYCLE                ),
    // .....
    .active_left_i                        (data_active_left[(COL_NUM-gi)*INPUTDW-1:((COL_NUM-1)-gi)*INPUTDW]),
    `ifdef FULL_PRECISION
    .score_row_o                          (out_sum[COL_NUM*OUTPUTDW*(gi+1)-1:COL_NUM*OUTPUTDW*gi]),
    `else
    `endif
    .score_row_o_q                        (out_sum_q[COL_NUM*OUTPUTDW*(gi+1)-1:COL_NUM*OUTPUTDW*gi]),
    .in_bias_above_i                      (in_bias_above             ),
    .in_weight_above_i                    (in_weight_above_t[gi]     ),
    .out_weight_below_o                   (out_weight_below[COL_NUM*INPUTDW*(gi+1)-1:COL_NUM*INPUTDW*gi]),
    `ifdef MAX_USE
    .data_max_row_o                       (data_max_sa[OUTPUTDW*(gi+1)-1:gi*OUTPUTDW]),
    `endif
    .PE_valid_o                           (PE_valid[gi]              ),
    .OS_valid_o                           (OS_valid[gi]              )
    );
    end
endgenerate
//------------------------OUTPUT SA STATE CNT/VALID------------------------
  logic       en_comflict_0_d;
  logic       [$clog2(ROW_NUM)-1: 0]cnt_o;
  logic       [$clog2(ROW_NUM)-1: 0]cnt_o_t;
  logic       [$clog2(ROW_NUM)-1: 0]cnt_o_switch;
  logic       comflict_flag;
  logic       valid_o;
  logic       cnt_o_clear,cnt_o_valid;
  `FF(en_comflict_0_d, en_comflict[0], '0, clk, rst_n)
  assign      comflict_flag = OS_valid[0] & en_comflict_0_d;
  assign      valid_o = (Flag_o | cnt_o!='d0 | comflict_flag ) & Flag_o_ready;
  assign      cnt_o_clear = (cnt_o == ($clog2(ROW_NUM))'(FINISH_ROW-1) && valid_o);
  assign      cnt_o_valid = (Flag_o_ready && valid_o);
  assign      cnt_o_t = cnt_o + 1;
  assign      cnt_o_switch = matrix_add_mode ? (FINISH_ROW-1-cnt_o) : cnt_o;
  `FFLARNC(cnt_o, cnt_o_t, cnt_o_valid, cnt_o_clear, '0, clk, rst_n)
  logic       [COL_NUM*OUTPUTDW-1: 0]out_sum_final_t;
  logic       [COL_NUM*QUANTDW-1: 0]out_sum_final_q_t;
  logic       row_score_valid_t;
  logic       [$clog2(ROW_NUM): 0] row_seq_o_t;
  logic       cal_finish_t;
  assign      out_sum_final_t = valid_o ? out_sum_q[(cnt_o_switch*COL_NUM*OUTPUTDW) +: (COL_NUM*OUTPUTDW)] : 'd0;
  assign      row_score_valid_t = valid_o;
  assign      row_seq_o_t = valid_o ? cnt_o : 'd0;
  assign      cal_finish_t = cnt_o_clear;
  `FFNR(row_score_valid, row_score_valid_t, clk)
  `FFNR(row_seq_o, row_seq_o_t, clk)
  `FFNR(out_sum_final_q, out_sum_final_q_t, clk)
  `FFNR(cal_finish, cal_finish_t, clk)
  generate
  for(genvar gj = 0; gj < COL_NUM; gj = gj + 1)
    begin:OUT_COL
        assign out_sum_final_q_t[(gj*QUANTDW)+:QUANTDW] = SA_pkg::sat_truncate_func(out_sum_final_t[(gj*OUTPUTDW)+:OUTPUTDW], cutbit, shift_mode);
    end
  endgenerate
  `ifdef FULL_PRECISION
  logic       [COL_NUM*OUTPUTDW-1: 0]out_sum_final_t;
  assign      out_sum_final_t = valid_o ? out_sum[(cnt_o*COL_NUM*OUTPUTDW) +: (COL_NUM*OUTPUTDW)] : 'd0;
  `FF(out_sum_final, out_sum_final_t, '0, clk, rst_n)
  `endif
  `ifdef MAX_USE
  logic [OUTPUTDW-1: 0]  data_max_row_t;
  assign data_max_row_t = valid_o & data_max_sa[(cnt_o*OUTPUTDW)+:OUTPUTDW];
  `FF(data_max_row_o, data_max_row_t, '0, clk, rst_n)
  `endif
endmodule