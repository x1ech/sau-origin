`timescale 1ns/1ps

module im2col_mikui_sau_pipeline #(
    parameter int ROWS = 16,
    parameter int COLS = 16,
    parameter int INPUT_W = 8,
    parameter int OUTPUT_W = 24,
    parameter int QUANT_W = 16,
    parameter int CNT_W = 10,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int FIFO_DEPTH = 4,
    parameter int MAX_K = 567
) (
    input  logic clk,
    input  logic rst_n,
    input  logic cfg_valid,
    input  logic start,
    input  logic [15:0] cfg_n,
    input  logic [15:0] cfg_c,
    input  logic [15:0] cfg_h,
    input  logic [15:0] cfg_w,
    input  logic [15:0] cfg_out_h,
    input  logic [15:0] cfg_out_w,
    input  logic [3:0] cfg_kernel_h,
    input  logic [3:0] cfg_kernel_w,
    input  logic [3:0] cfg_stride_h,
    input  logic [3:0] cfg_stride_w,
    input  logic [3:0] cfg_dilation_h,
    input  logic [3:0] cfg_dilation_w,
    input  logic [15:0] cfg_pad_top,
    input  logic [15:0] cfg_pad_left,
    input  logic [15:0] cfg_spad_base,
    input  logic [15:0] cfg_out_channels,
    input  logic [4:0] cfg_cutbit,
    input  logic [1:0] cfg_weight_generator,
    input  logic cfg_bias_zero,
    input  logic [31:0] cfg_expected_tiles,
    input  logic output_grant,

    output logic [15:0] sram_req_valid,
    output logic [15:0][11:0] sram_req_addr,
    input  logic [15:0] sram_resp_valid,
    input  logic [15:0][7:0] sram_resp_data,

    output logic [2:0] pipeline_state,
    output logic [31:0] tile_index,
    output logic [15:0] tile_buffer_count,
    output logic [15:0] collect_k,
    output logic [15:0] stream_k,
    output logic im2col_feed_ready,
    output logic output_request,
    output logic drained,

    output logic sa_ins_valid,
    output logic sa_input_valid,
    output logic [10:0] sa_calc_cycles,
    output logic [4:0] sa_valid_rows,
    output logic [4:0] sa_valid_columns,
    output logic [4:0] sa_cutbit,
    output logic [255:0] sa_biases,
    output logic [127:0] sa_activations,
    output logic [127:0] sa_weights,
    output logic [15:0] sa_row_mask,
    output logic [15:0] sa_column_mask,

    output logic [255:0] pe_valid_mask,
    output logic [255:0] pe_mac_commit_mask,
    output logic [255:0] pe_add_commit_mask,
    output logic [2047:0] pe_activations,
    output logic [2047:0] pe_weights,
    output logic [6143:0] pe_accumulators,
    output logic [15:0] row_ready_mask,

    output logic [255:0] output_slots,
    output logic row_score_valid,
    output logic [4:0] row_sequence,
    output logic storage_ready,
    output logic pe_finish,
    output logic cal_finish
);
    localparam int SP_ROW_BITS = $clog2(SP_BANK_ENTRIES);
    localparam int SP_ADDR_BITS = 4 + SP_ROW_BITS;

    typedef enum logic [2:0] {
        P_IDLE = 3'd0,
        P_COLLECT_TILE = 3'd1,
        P_LAUNCH_SA = 3'd2,
        P_STREAM_K = 3'd3,
        P_WAIT_RESULT = 3'd4,
        P_DRAIN_OUTPUT = 3'd5,
        P_DONE = 3'd6
    } pipeline_state_t;

    pipeline_state_t state_q;
    logic [127:0] tile_data [0:MAX_K-1];
    logic [15:0] tile_mask [0:MAX_K-1];
    logic [15:0] k_q;
    logic [15:0] collect_q;
    logic [15:0] stream_q;
    logic [31:0] completed_tiles_q;
    logic im2col_done_seen_q;

    logic im2col_busy;
    logic im2col_done;
    logic im2col_feed_valid;
    logic [127:0] im2col_feed_data;
    logic [15:0] im2col_feed_mask;

    logic sa_internal_valid;
    logic [3:0] sa_output_counter;
    logic [15:0] sa_os_valid;
    logic [10:0] sa_datain_count;
    logic array_start_ready;
    logic array_input_ready;
    logic array_output_valid;
    logic [4:0] array_output_row;
    logic [255:0] array_output_data;
    logic array_busy;
    logic array_done;
    logic [2:0] array_debug_state;
    logic [255:0] array_debug_mac_commit_mask;
    logic [255:0] array_debug_add_commit_mask;
    logic [2047:0] array_debug_activations;
    logic [2047:0] array_debug_weights;
    logic [6143:0] array_debug_accumulators;

    function automatic [7:0] weight_value(
        input int generator,
        input int output_channel,
        input int channel,
        input int kernel_h,
        input int kernel_w
    );
        integer raw;
        integer signed value;
        begin
            if (generator == 1)
                value = 0;
            else if (generator == 2)
                value = 1;
            else begin
                raw = (output_channel * 29 + channel * 17 +
                       kernel_h * 5 + kernel_w * 3 + 11) % 255;
                value = raw - 127;
            end
            weight_value = value[7:0];
        end
    endfunction

    function automatic [15:0] bias_value(
        input bit zero_bias,
        input int output_channel
    );
        integer signed value;
        begin
            if (zero_bias)
                value = 0;
            else
                value = ((output_channel * 37 + 13) % 257) - 128;
            bias_value = value[15:0];
        end
    endfunction

    function automatic [4:0] mask_population(input logic [15:0] mask);
        integer count;
        begin
            count = 0;
            for (int lane = 0; lane < ROWS; lane++)
                count += mask[lane];
            mask_population = count[4:0];
        end
    endfunction

    gemmini_im2col_chw_gather_readable #(
        .BLOCK_SIZE(ROWS),
        .ELEM_W(INPUT_W),
        .SP_BANKS(ROWS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES),
        .FIFO_DEPTH(FIFO_DEPTH)
    ) im2col_dut (
        .clk(clk),
        .rst_n(rst_n),
        .cfg_valid(cfg_valid),
        .cfg_spad_base(cfg_spad_base[SP_ADDR_BITS-1:0]),
        .cfg_n(cfg_n),
        .cfg_c(cfg_c),
        .cfg_h(cfg_h),
        .cfg_w(cfg_w),
        .cfg_out_h(cfg_out_h),
        .cfg_out_w(cfg_out_w),
        .cfg_kernel_h(cfg_kernel_h),
        .cfg_kernel_w(cfg_kernel_w),
        .cfg_stride_h(cfg_stride_h),
        .cfg_stride_w(cfg_stride_w),
        .cfg_dilation_h(cfg_dilation_h),
        .cfg_dilation_w(cfg_dilation_w),
        .cfg_pad_top(cfg_pad_top),
        .cfg_pad_left(cfg_pad_left),
        .cfg_dw_mode(1'b0),
        .cfg_kernel_pattern(16'hffff),
        .start(start),
        .busy(im2col_busy),
        .done(im2col_done),
        .sram_req_valid(sram_req_valid),
        .sram_req_addr(sram_req_addr),
        .sram_resp_valid(sram_resp_valid),
        .sram_resp_data(sram_resp_data),
        .feed_valid(im2col_feed_valid),
        .feed_ready(im2col_feed_ready),
        .feed_data(im2col_feed_data),
        .feed_mask(im2col_feed_mask)
    );

    sau_array_16x16 #(
        .SIZE(ROWS),
        .INPUT_W(INPUT_W),
        .ACC_W(OUTPUT_W),
        .BIAS_W(QUANT_W),
        .OUTPUT_W(QUANT_W),
        .K_W(11)
    ) sa_dut (
        .clk(clk),
        .rst_n(rst_n),
        .start(sa_ins_valid),
        .start_ready(array_start_ready),
        .cfg_k(sa_calc_cycles),
        .cfg_rows(sa_valid_rows),
        .cfg_cols(sa_valid_columns),
        .cfg_cutbit(sa_cutbit),
        .cfg_bias(sa_biases),
        .input_valid(sa_input_valid),
        .input_ready(array_input_ready),
        .activation_data(sa_activations),
        .weight_data(sa_weights),
        .output_valid(array_output_valid),
        .output_ready(output_grant),
        .output_row(array_output_row),
        .output_data(array_output_data),
        .busy(array_busy),
        .done(array_done),
        .debug_state(array_debug_state),
        .debug_mac_commit_mask(array_debug_mac_commit_mask),
        .debug_add_commit_mask(array_debug_add_commit_mask),
        .debug_activation_packed(array_debug_activations),
        .debug_weight_packed(array_debug_weights),
        .debug_acc_packed(array_debug_accumulators)
    );

    always_comb begin
        pipeline_state = state_q;
        tile_index = completed_tiles_q;
        tile_buffer_count = collect_q;
        collect_k = collect_q;
        stream_k = stream_q;
        k_q = cfg_c * 9;
        im2col_feed_ready = state_q == P_COLLECT_TILE && collect_q < k_q;

        sa_ins_valid = state_q == P_LAUNCH_SA;
        sa_input_valid = state_q == P_STREAM_K;
        sa_calc_cycles = k_q[10:0];
        sa_valid_rows = collect_q == 0 ? 5'd0 :
            mask_population(tile_mask[0]);
        sa_valid_columns = cfg_out_channels[4:0];
        sa_cutbit = cfg_cutbit;

        sa_activations = '0;
        sa_weights = '0;
        sa_biases = '0;
        sa_row_mask = '0;
        sa_column_mask = '0;
        for (int column = 0; column < COLS; column++) begin
            if (column < cfg_out_channels) begin
                sa_biases[column*16 +: 16] =
                    bias_value(cfg_bias_zero, column);
            end
        end
        if (sa_input_valid) begin
            sa_activations = tile_data[stream_q];
            sa_row_mask = tile_mask[0];
            sa_column_mask = cfg_out_channels == 16 ? 16'hffff :
                (16'h0001 << cfg_out_channels) - 1'b1;
            for (int column = 0; column < COLS; column++) begin
                if (column < cfg_out_channels) begin
                    sa_weights[column*8 +: 8] = weight_value(
                        cfg_weight_generator, column, stream_q / 9,
                        (stream_q % 9) / 3, stream_q % 3);
                end
            end
        end

        output_request = array_output_valid;
        sa_internal_valid = array_output_valid && output_grant;
        sa_output_counter = array_output_row[3:0];
        sa_os_valid = array_output_valid ?
            (16'h0001 << array_output_row) : 16'h0000;
        output_slots = array_output_data;
        row_score_valid = array_output_valid && output_grant;
        row_sequence = row_score_valid ? array_output_row : 5'd0;
        storage_ready = array_output_valid;
        pe_finish = array_output_valid && array_output_row == 0;
        cal_finish = array_done;
        sa_datain_count = stream_q[10:0];
        pe_valid_mask = array_debug_mac_commit_mask;
        pe_mac_commit_mask = array_debug_mac_commit_mask;
        pe_add_commit_mask = array_debug_add_commit_mask;
        pe_activations = array_debug_activations;
        pe_weights = array_debug_weights;
        pe_accumulators = '0;
        for (int pe = 0; pe < ROWS * COLS; pe++) begin
            if (array_debug_mac_commit_mask[pe] ||
                array_debug_add_commit_mask[pe])
                pe_accumulators[pe*OUTPUT_W +: OUTPUT_W] =
                    array_debug_accumulators[pe*OUTPUT_W +: OUTPUT_W];
        end
        drained = state_q == P_DONE && im2col_done_seen_q &&
            im2col_dut.fifo_count == 0 && collect_q == 0 &&
            !array_busy &&
            completed_tiles_q == cfg_expected_tiles;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_q <= P_IDLE;
            collect_q <= 0;
            stream_q <= 0;
            completed_tiles_q <= 0;
            im2col_done_seen_q <= 1'b0;
            row_ready_mask <= '0;
            for (int index = 0; index < MAX_K; index++) begin
                tile_data[index] <= '0;
                tile_mask[index] <= '0;
            end
        end else begin
            if (im2col_done)
                im2col_done_seen_q <= 1'b1;
            row_ready_mask <= row_ready_mask | sa_os_valid;
            if (cal_finish) begin
                row_ready_mask <= '0;
            end

            case (state_q)
                P_IDLE: begin
                    if (start) begin
                        state_q <= P_COLLECT_TILE;
                        collect_q <= 0;
                        stream_q <= 0;
                        completed_tiles_q <= 0;
                        im2col_done_seen_q <= 1'b0;
                    end
                end
                P_COLLECT_TILE: begin
                    if (im2col_feed_valid && im2col_feed_ready) begin
                        tile_data[collect_q] <= im2col_feed_data;
                        tile_mask[collect_q] <= im2col_feed_mask;
                        collect_q <= collect_q + 1'b1;
                        if (collect_q + 1'b1 == k_q)
                            state_q <= P_LAUNCH_SA;
                    end
                end
                P_LAUNCH_SA: begin
                    stream_q <= 0;
                    state_q <= P_STREAM_K;
                end
                P_STREAM_K: begin
                    stream_q <= stream_q + 1'b1;
                    if (stream_q + 1'b1 == k_q)
                        state_q <= P_WAIT_RESULT;
                end
                P_WAIT_RESULT: begin
                    if (array_output_valid)
                        state_q <= P_DRAIN_OUTPUT;
                end
                P_DRAIN_OUTPUT: begin
                    if (cal_finish) begin
                        completed_tiles_q <= completed_tiles_q + 1'b1;
                        collect_q <= 0;
                        stream_q <= 0;
                        for (int index = 0; index < MAX_K; index++) begin
                            tile_data[index] <= '0;
                            tile_mask[index] <= '0;
                        end
                        if (completed_tiles_q + 1'b1 == cfg_expected_tiles)
                            state_q <= P_DONE;
                        else
                            state_q <= P_COLLECT_TILE;
                    end
                end
                P_DONE: state_q <= P_DONE;
                default: state_q <= P_IDLE;
            endcase
        end
    end

endmodule
