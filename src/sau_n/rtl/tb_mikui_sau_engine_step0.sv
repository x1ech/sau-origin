`timescale 1ns/1ps

module tb_mikui_sau_engine_step0;
  import SA_pkg::*;

  localparam int ROWS = 16;
  localparam int COLS = 16;
  localparam int INPUT_W = 8;
  localparam int OUTPUT_W = 24;
  localparam int QUANT_W = 16;
  localparam int CNT_W = 10;
  localparam int WATCHDOG_CYCLES = 5000;

  logic clk = 1'b0;
  logic rst_n = 1'b0;
  logic EN_i = 1'b0;
  logic Flag_o = 1'b0;
  logic Flag_o_ready = 1'b1;
  logic ins_valid_i = 1'b0;
  logic [1:0] sa_calmode_i = 2'b01;
  logic [1:0] sa_flowmode_i = 2'b00;
  logic [1:0] register_mode_i = 2'b00;
  logic shift_mode_i = 1'b0;
  logic [CNT_W:0] CALC_CYCLE_i = '0;
  logic [$clog2(ROWS):0] row_num_i = '0;
  logic [$clog2(COLS):0] col_num_i = '0;
  logic [$clog2(OUTPUT_W)-1:0] cutbit = '0;
  logic shift_ctl_i = 1'b0;
  logic [ROWS*INPUT_W-1:0] data_active_left = '0;
  logic [COLS*INPUT_W-1:0] in_weight_above = '0;
  logic [COLS*2*INPUT_W-1:0] in_bias_above = '0;

  logic [COLS*QUANT_W-1:0] out_sum_final_q;
  logic row_score_valid;
  logic [$clog2(ROWS):0] row_seq_o;
  logic storage_ready;
  logic pe_finish_o;
  logic cal_finish;

  string case_name;
  string trace_path;
  integer expect_patched;
  integer trace_fd;
  integer trace_cycle = 0;
  integer watchdog_cycle = 0;
  integer requested_rows;
  integer requested_cols;
  integer k_cycles;
  integer drive_backpressure;
  integer test_active = 0;
  integer trace_active = 0;

  wire [ROWS*COLS*OUTPUT_W-1:0] pe_acc_packed;
  wire [ROWS*COLS-1:0] pe_add_commit_mask;
  wire [ROWS*COLS-1:0] pe_mac_commit_mask;

  always #5 clk = ~clk;

  SA_ENGINE #(
    .ROW_NUM(ROWS),
    .COL_NUM(COLS),
    .OUTPUTDW(OUTPUT_W),
    .CNT_DW(CNT_W),
    .INPUTDW(INPUT_W),
    .QUANTDW(QUANT_W)
  ) dut (
    .clk(clk),
    .rst_n(rst_n),
    .EN_i(EN_i),
    .Flag_o(Flag_o),
    .Flag_o_ready(Flag_o_ready),
    .ins_valid_i(ins_valid_i),
    .sa_calmode_i(sa_calmode_i),
    .sa_flowmode_i(sa_flowmode_i),
    .register_mode_i(register_mode_i),
    .shift_mode_i(shift_mode_i),
    .CALC_CYCLE_i(CALC_CYCLE_i),
    .row_num_i(row_num_i),
    .col_num_i(col_num_i),
    .cutbit(cutbit),
    .shift_ctl_i(shift_ctl_i),
    .data_active_left(data_active_left),
    .in_weight_above(in_weight_above),
    .in_bias_above(in_bias_above),
    .out_sum_final_q(out_sum_final_q),
    .row_score_valid(row_score_valid),
    .row_seq_o(row_seq_o),
    .storage_ready(storage_ready),
    .pe_finish_o(pe_finish_o),
    .cal_finish(cal_finish)
  );

  generate
    for (genvar r = 0; r < ROWS; r++) begin : OBS_ROW
      for (genvar c = 0; c < COLS; c++) begin : OBS_COL
        localparam int PE_INDEX = r * COLS + c;
        assign pe_acc_packed[PE_INDEX*OUTPUT_W +: OUTPUT_W] =
          dut.PE_row[r].PE_row_unit.PE_COL[c].PE_unit.data_out_sum_tmp;
        assign pe_add_commit_mask[PE_INDEX] =
          dut.PE_row[r].PE_row_unit.PE_COL[c].PE_unit.add_state_valid;
        assign pe_mac_commit_mask[PE_INDEX] =
          dut.PE_row[r].PE_row_unit.PE_COL[c].PE_unit.mac_en[1];
      end
    end
  endgenerate

  task automatic fail(input string message);
    begin
      $display("FAIL tb_mikui_sau_engine_step0 case=%s variant=%0d: %s",
               case_name, expect_patched, message);
      if (trace_fd != 0)
        $fclose(trace_fd);
      $fatal(1);
    end
  endtask

  task automatic set_case_parameters;
    begin
      requested_rows = 1;
      requested_cols = 1;
      k_cycles = 9;
      drive_backpressure = 0;
      cutbit = 5'd0;

      if (case_name == "tail_r1_c1") begin
        requested_rows = 1;
        requested_cols = 1;
      end else if (case_name == "tail_r15_c15") begin
        requested_rows = 15;
        requested_cols = 15;
        cutbit = 5'd5;
      end else if (case_name == "tail_r16_c16") begin
        requested_rows = 16;
        requested_cols = 16;
        cutbit = 5'd5;
      end else if (case_name == "mapping_k9") begin
        requested_rows = 16;
        requested_cols = 16;
        cutbit = 5'd5;
      end else if (case_name == "control_k9_bp") begin
        requested_rows = 3;
        requested_cols = 3;
        cutbit = 5'd2;
        drive_backpressure = 1;
      end else if (case_name == "sat_pos_k567") begin
        requested_rows = 1;
        requested_cols = 1;
        k_cycles = 567;
        cutbit = 5'd16;
      end else if (case_name == "sat_neg_k567") begin
        requested_rows = 1;
        requested_cols = 1;
        k_cycles = 567;
        cutbit = 5'd16;
      end else begin
        fail($sformatf("unknown +CASE=%s", case_name));
      end

      row_num_i = requested_rows;
      col_num_i = requested_cols;
      CALC_CYCLE_i = k_cycles;
    end
  endtask

  task automatic drive_payload(input integer k_index);
    integer r;
    integer c;
    integer activation_value;
    integer weight_value;
    integer bias_value;
    begin
      data_active_left = '0;
      in_weight_above = '0;
      in_bias_above = '0;

      for (r = 0; r < ROWS; r = r + 1) begin
        if (case_name == "sat_pos_k567" ||
            case_name == "sat_neg_k567")
          activation_value = (r == 0) ? -128 : 0;
        else
          activation_value = r + 1;
        data_active_left[(ROWS-1-r)*INPUT_W +: INPUT_W] =
          activation_value[INPUT_W-1:0];
      end

      for (c = 0; c < COLS; c = c + 1) begin
        if (case_name == "sat_pos_k567")
          weight_value = (c == 0) ? -128 : 0;
        else if (case_name == "sat_neg_k567")
          weight_value = (c == 0) ? 127 : 0;
        else
          weight_value = c + 1;
        in_weight_above[c*INPUT_W +: INPUT_W] =
          weight_value[INPUT_W-1:0];

        if (case_name == "sat_pos_k567" ||
            case_name == "sat_neg_k567")
          bias_value = 0;
        else
          bias_value = c - 8;
        in_bias_above[c*2*INPUT_W +: 2*INPUT_W] =
          bias_value[2*INPUT_W-1:0];
      end

      if (k_index < 0)
        fail("negative k index");
    end
  endtask

  always @(negedge clk) begin
    #1;
    if (trace_active) begin
      $fwrite(trace_fd,
        "%0d,%s,%0d,%0d,%0d,%0d,%0d,%0d,%0h,%0h,%0h,%0d,%0d,%0d,%0d,%0d,%032h,%032h,%064h,%0h,%0d,%0d,%0d,%04h,%04h,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%064h,%064h,%01536h\n",
        trace_cycle, case_name, expect_patched, rst_n, EN_i, Flag_o,
        Flag_o_ready, ins_valid_i, sa_calmode_i, sa_flowmode_i,
        register_mode_i, shift_mode_i, shift_ctl_i, CALC_CYCLE_i,
        row_num_i, col_num_i, data_active_left, in_weight_above,
        in_bias_above, dut.sa_cur_state, dut.FINISH_ROW, dut.FINISH_COL,
        dut.datain_cnt, dut.OS_valid, dut.PE_valid, storage_ready,
        dut.valid_o, dut.cnt_o, row_score_valid, row_seq_o, cal_finish,
        out_sum_final_q, pe_mac_commit_mask, pe_add_commit_mask,
        pe_acc_packed);
      trace_cycle = trace_cycle + 1;
    end

    if (test_active && drive_backpressure)
      Flag_o_ready = ((trace_cycle % 5) != 1);
    else
      Flag_o_ready = 1'b1;
  end

  always @(posedge clk) begin
    if (test_active) begin
      watchdog_cycle = watchdog_cycle + 1;
      if (watchdog_cycle > WATCHDOG_CYCLES)
        fail("watchdog expired");
    end
  end

  initial begin
    trace_fd = 0;
    if (!$value$plusargs("CASE=%s", case_name))
      case_name = "tail_r1_c1";
    if (!$value$plusargs("TRACE=%s", trace_path))
      trace_path = "step0_trace.csv";
    if (!$value$plusargs("EXPECT_PATCHED=%d", expect_patched))
      expect_patched = 0;

    trace_fd = $fopen(trace_path, "w");
    if (trace_fd == 0)
      fail($sformatf("cannot open trace %s", trace_path));
    $fwrite(trace_fd,
      "cycle,case_name,expect_patched,rst_n,en_i,flag_o,flag_o_ready,ins_valid,calmode,flowmode,register_mode,shift_mode,shift_ctl,calc_cycle,row_num,col_num,activation_bus,weight_bus,bias_bus,sa_state,finish_row,finish_col,datain_cnt,os_valid,pe_valid,storage_ready,internal_valid,cnt_o,row_score_valid,row_seq,cal_finish,output_bus,pe_mac_commit_mask,pe_add_commit_mask,pe_acc_packed\n");

    set_case_parameters();
    repeat (5) @(negedge clk);
    rst_n = 1'b1;
    trace_active = 1;
    test_active = 1;

    @(negedge clk);
    ins_valid_i = 1'b1;
    @(negedge clk);
    ins_valid_i = 1'b0;

    for (integer k = 0; k < k_cycles; k = k + 1) begin
      drive_payload(k);
      EN_i = 1'b1;
      @(negedge clk);
    end
    EN_i = 1'b0;
    data_active_left = '0;
    in_weight_above = '0;

    while (!storage_ready)
      @(negedge clk);

    Flag_o = 1'b1;
    while (!dut.valid_o)
      @(negedge clk);
    Flag_o = 1'b0;

    while (!cal_finish)
      @(negedge clk);
    repeat (3) @(negedge clk);

    test_active = 0;
    trace_active = 0;
    $fclose(trace_fd);
    trace_fd = 0;
    $display("PASS tb_mikui_sau_engine_step0 case=%s variant=%0d trace=%s",
             case_name, expect_patched, trace_path);
    $finish;
  end
endmodule
